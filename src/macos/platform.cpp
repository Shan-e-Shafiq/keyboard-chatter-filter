// SPDX-License-Identifier: MIT
//
// macOS implementation of the CLI commands: a per-user LaunchAgent runs `keyboard-chatter-filter
// run` at login; everything here manages that agent and talks to the running daemon.
#include "app/platform.h"

#include <chrono>
#include <climits>
#include <csignal>
#include <stdexcept>
#include <iostream>
#include <thread>
#include <unistd.h>

#include "common/logging.h"
#include "common/posix/posix.h"
#include "common/status.h"
#include "kcf/build_info.h"
#include "macos/daemon.h"
#include "macos/keyboard_interceptor.h"
#include "macos/launch_agent.h"
#include "macos/paths.h"

namespace kcf::macos {

std::optional<Paths> user_paths() {
    const auto home = posix::home_directory();
    if (!home) {
        return std::nullopt;
    }
    Paths p;
    // Always ~/.config (not $XDG_CONFIG_HOME): launchd does not pass the login shell's environment,
    // so the daemon and the CLI must agree on a fixed location.
    p.config_file = *home / ".config" / build::kProgramName / "config.toml";
    p.state_dir = *home / "Library" / "Application Support" / build::kProgramName;
    p.lock_file = p.state_dir / "daemon.lock";
    p.status_socket = p.state_dir / "status.sock";
    // Unix socket paths are limited to 104 bytes. With an unusually long home path, fall back to
    // the per-user temporary directory (private to the user, same for the daemon and the CLI).
    if (p.status_socket.string().size() >= 100) {
        char temp[PATH_MAX] = {};
        if (::confstr(_CS_DARWIN_USER_TEMP_DIR, temp, sizeof temp) > 0) {
            p.status_socket = std::filesystem::path(temp) / (std::string(build::kProgramName) + ".sock");
        }
    }
    p.log_dir = *home / "Library" / "Logs" / build::kProgramName;
    p.log_file = p.log_dir / (std::string(build::kProgramName) + ".log");
    p.launch_agent = *home / "Library" / "LaunchAgents" / (launch_agent_label() + ".plist");
    return p;
}

namespace {

Paths require_paths() {
    auto p = user_paths();
    if (!p) {
        throw std::runtime_error("cannot determine the home directory");
    }
    return *p;
}

std::optional<DaemonStatus> query_daemon(const Paths& paths) {
    const auto raw = posix::query_status_socket(paths.status_socket, std::chrono::milliseconds(1000));
    if (!raw) {
        return std::nullopt;
    }
    return parse_status(*raw);
}

// Waits until the daemon answers on its status socket (it was just started).
std::optional<DaemonStatus> wait_for_daemon(const Paths& paths, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto status = query_daemon(paths)) {
            if (status->state != DaemonState::Starting) {
                return status;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return query_daemon(paths);
}

void print_permission_help() {
    const auto exe = posix::current_executable();
    std::cout << "\nmacOS requires Accessibility permission before any app may filter keyboard input:\n"
              << "  1. Open System Settings > Privacy & Security > Accessibility\n"
              << "  2. Switch on '" << build::kProgramName << "'. If it is missing, click '+', press Cmd+Shift+G\n"
              << "     and enter: " << (exe ? exe->string() : std::string(build::kProgramName)) << '\n'
              << "Filtering starts by itself a few seconds after access is granted.\n"
              << "Shortcut: open \"x-apple.systempreferences:com.apple.preference.security?Privacy_Accessibility\"\n";
}

int report_started(const Paths& paths) {
    const auto status = wait_for_daemon(paths, std::chrono::seconds(5));
    if (!status) {
        std::cerr << "error: the service was started but is not responding. Check the log: " << paths.log_file.string()
                  << '\n';
        return 1;
    }
    std::cout << "keyboard-chatter-filter is running.\n" << describe_status(*status);
    if (status->state == DaemonState::WaitingForPermission) {
        print_permission_help();
    }
    return 0;
}

bool remove_known_file(const std::filesystem::path& path) {
    std::string error;
    if (!posix::remove_file_if_exists(path, error)) {
        std::cerr << "warning: " << error << '\n';
        return false;
    }
    return true;
}

void remove_dir_if_empty(const std::filesystem::path& dir) {
    std::error_code ec;
    if (std::filesystem::is_directory(dir, ec) && std::filesystem::is_empty(dir, ec)) {
        std::filesystem::remove(dir, ec);
    }
}

// Only ever deletes a regular file called exactly "keyboard-chatter-filter".
bool remove_binary(const std::filesystem::path& path) {
    std::error_code ec;
    if (path.filename() != build::kProgramName || !std::filesystem::is_regular_file(path, ec)) {
        return false;
    }
    std::string error;
    if (!posix::remove_file_if_exists(path, error)) {
        std::cerr << "warning: " << error << '\n';
        return false;
    }
    return true;
}

}  // namespace
}  // namespace kcf::macos

namespace kcf::platform {

using namespace kcf::macos;

std::filesystem::path default_config_path() {
    return require_paths().config_file;
}

int run(const CliOptions& options) {
    DaemonOptions daemon_options;
    daemon_options.config_path = options.config_path.value_or(default_config_path());
    daemon_options.log_level_override = options.log_level;
    daemon_options.dry_run = options.dry_run;
    return run_daemon(daemon_options);
}

int install(const CliOptions&) {
    const Paths paths = require_paths();
    const auto exe = posix::current_executable();
    if (!exe) {
        std::cerr << "error: cannot determine the path of this executable\n";
        return 1;
    }

    std::string error;
    if (!posix::ensure_directory(paths.log_dir, 0700, error) ||
        !posix::ensure_directory(paths.state_dir, 0700, error) ||
        !posix::ensure_directory(paths.config_file.parent_path(), 0700, error) ||
        !posix::ensure_directory(paths.launch_agent.parent_path(), 0755, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    // Pre-create the log so launchd appends to a private file instead of creating a readable one.
    if (!std::filesystem::exists(paths.log_file) && !posix::write_file_atomic(paths.log_file, "", 0600, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    if (!std::filesystem::exists(paths.config_file)) {
        if (!posix::write_file_atomic(paths.config_file, default_configuration_text(), 0600, error)) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        std::cout << "Created configuration file " << paths.config_file.string() << '\n';
    }

    // Reinstalling: unload the old definition first so launchd picks up the new one.
    if (!bootout(error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    if (!posix::write_file_atomic(paths.launch_agent, render_launch_agent(*exe, paths.log_file), 0644, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    std::cout << "Installed LaunchAgent " << paths.launch_agent.string() << " (starts at login)\n";
    if (!bootstrap(paths.launch_agent, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    return report_started(paths);
}

int start() {
    const Paths paths = require_paths();
    if (!std::filesystem::exists(paths.launch_agent)) {
        std::cerr << "error: the service is not installed. Run 'keyboard-chatter-filter install' first.\n";
        return 1;
    }
    std::string error;
    const bool ok = query_job().loaded ? kickstart(false, error) : bootstrap(paths.launch_agent, error);
    if (!ok) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    return report_started(paths);
}

int stop() {
    std::string error;
    if (!bootout(error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    std::cout << "Stopped. The keyboard is unfiltered until the next login or "
                 "'keyboard-chatter-filter start'.\n";
    return 0;
}

int restart() {
    const Paths paths = require_paths();
    if (!query_job().loaded) {
        return start();
    }
    std::string error;
    if (!kickstart(true, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    return report_started(paths);
}

int reload() {
    const Paths paths = require_paths();
    std::string error;
    if (!query_job().loaded) {
        // Perhaps a foreground `run`: signal the lock holder directly.
        if (const auto pid = posix::InstanceLock::holder(paths.lock_file); pid && ::kill(*pid, SIGHUP) == 0) {
            std::cout << "Asked the running filter (pid " << *pid << ") to reload its configuration.\n";
            return 0;
        }
        std::cerr << "error: the filter is not running\n";
        return 1;
    }
    if (!send_signal("SIGHUP", error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    std::cout << "Configuration reload requested. Problems, if any, are reported in " << paths.log_file.string()
              << '\n';
    return 0;
}

int status() {
    const Paths paths = require_paths();
    const bool installed = std::filesystem::exists(paths.launch_agent);
    const LaunchdJob job = query_job();
    if (const auto daemon = query_daemon(paths)) {
        std::cout << "keyboard-chatter-filter is running" << (job.loaded ? "" : " (in the foreground)") << ".\n"
                  << describe_status(*daemon);
        if (!installed) {
            std::cout << "  Autostart:   not installed\n";
        }
        if (job.loaded) {
            std::cout << "  Log:         " << paths.log_file.string() << '\n';
        }
        if (daemon->state == DaemonState::WaitingForPermission) {
            print_permission_help();
        }
        return daemon->state == DaemonState::Active || daemon->state == DaemonState::Disabled ? 0 : 1;
    }
    if (const auto pid = posix::InstanceLock::holder(paths.lock_file)) {
        std::cout << "keyboard-chatter-filter is running (pid " << *pid << ") but not answering status requests.\n";
        return 1;
    }
    if (!installed) {
        std::cout << "keyboard-chatter-filter is not installed.\n";
        return 3;
    }
    if (job.loaded && job.pid) {
        std::cout << "keyboard-chatter-filter is running (pid " << *job.pid
                  << ") but not answering status requests. Check " << paths.log_file.string() << '\n';
        return 1;
    }
    std::cout << "keyboard-chatter-filter is installed but not running"
              << (job.loaded ? " (launchd will retry; see the log)" : ". Start it with 'keyboard-chatter-filter start'")
              << ".\n  Log: " << paths.log_file.string() << '\n';
    return 3;
}

int uninstall(const CliOptions& options) {
    const Paths paths = require_paths();
    std::string error;
    int rc = 0;

    const auto installed_binary = launch_agent_executable(paths.launch_agent);
    if (!bootout(error)) {
        std::cerr << "warning: " << error << '\n';
        rc = 1;
    }
    const bool had_agent = std::filesystem::exists(paths.launch_agent);
    if (!remove_known_file(paths.launch_agent)) {
        rc = 1;
    } else if (had_agent) {
        std::cout << "Removed LaunchAgent " << paths.launch_agent.string() << '\n';
    }

    // Runtime state is not user data: always clean it up.
    remove_known_file(paths.status_socket);
    remove_known_file(paths.lock_file);
    remove_dir_if_empty(paths.state_dir);

    if (options.purge) {
        for (const auto& file : {paths.config_file, paths.log_file, std::filesystem::path(paths.log_file.string() + ".1")}) {
            if (std::filesystem::exists(file) && remove_known_file(file)) {
                std::cout << "Removed " << file.string() << '\n';
            }
        }
        remove_dir_if_empty(paths.config_file.parent_path());
        remove_dir_if_empty(paths.log_dir);
    } else if (std::filesystem::exists(paths.config_file)) {
        std::cout << "Kept " << paths.config_file.string() << " and logs in " << paths.log_dir.string()
                  << " (use --purge to remove them)\n";
    }

    if (installed_binary && remove_binary(*installed_binary)) {
        std::cout << "Removed " << installed_binary->string() << '\n';
    }
    std::cout << "keyboard-chatter-filter has been uninstalled. You can also remove it from System Settings > "
                 "Privacy & Security > Accessibility.\n";
    return rc;
}

std::optional<int> run_platform_command(const CliOptions&) {
    return std::nullopt;
}

}  // namespace kcf::platform
