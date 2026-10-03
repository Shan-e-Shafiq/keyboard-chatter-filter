// SPDX-License-Identifier: MIT
//
// Linux implementation of the CLI commands. A systemd system service runs `keyboard-chatter-filter
// run` from boot; these commands manage it with systemctl (executed directly, never via a shell).
#include "app/platform.h"

#include <chrono>
#include <csignal>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "common/logging.h"
#include "common/posix/posix.h"
#include "common/status.h"
#include "common/text.h"
#include "kcf/build_info.h"
#include "kcf/embedded_templates.h"
#include "linux/daemon.h"

namespace kcf::linux_input {
namespace {

const std::filesystem::path kConfigDir = std::filesystem::path("/etc") / build::kProgramName;
const std::filesystem::path kModulesLoadFile =
    std::filesystem::path("/etc/modules-load.d") / (std::string(build::kServiceId) + ".conf");

std::filesystem::path unit_path() {
    return std::filesystem::path("/etc/systemd/system") / (std::string(build::kServiceId) + ".service");
}

std::string unit_name() {
    return std::string(build::kServiceId) + ".service";
}

std::optional<std::string> systemctl_path() {
    return posix::find_executable({"/usr/bin/systemctl", "/bin/systemctl"});
}

posix::ProcessResult systemctl(std::vector<std::string> args, bool capture = true) {
    const auto path = systemctl_path();
    if (!path) {
        return {-1, "systemctl not found; this installer supports systemd-based distributions"};
    }
    args.insert(args.begin(), *path);
    return posix::run_process(args, capture);
}

bool require_root(const char* what) {
    if (::geteuid() == 0) {
        return true;
    }
    std::cerr << "error: " << what << " needs root. Run: sudo " << build::kProgramName << ' ' << what << '\n';
    return false;
}

std::string first_line(const std::string& s) {
    const std::string trimmed(text::trim(s));
    return trimmed.substr(0, trimmed.find('\n'));
}

// A root service must never execute a file an unprivileged user could replace. Checks the binary
// and every directory above it.
bool path_is_root_controlled(const std::filesystem::path& file, std::string& problem) {
    std::filesystem::path p = file;
    while (true) {
        struct stat st {};
        if (::lstat(p.c_str(), &st) != 0) {
            problem = "cannot inspect " + p.string();
            return false;
        }
        if (st.st_uid != 0) {
            problem = p.string() + " is not owned by root";
            return false;
        }
        const bool sticky_dir = S_ISDIR(st.st_mode) && (st.st_mode & S_ISVTX) != 0;
        if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 && !sticky_dir) {
            problem = p.string() + " is writable by non-root users";
            return false;
        }
        if (sticky_dir) {
            problem = p.string() + " is a shared temporary directory";
            return false;
        }
        if (!p.has_parent_path() || p.parent_path() == p) {
            return true;
        }
        p = p.parent_path();
    }
}

std::optional<DaemonStatus> query_daemon() {
    for (const RuntimePaths& paths : {system_runtime_paths(), runtime_paths_for_current_user()}) {
        if (const auto raw = posix::query_status_socket(paths.status_socket, std::chrono::milliseconds(1000))) {
            return parse_status(*raw);
        }
    }
    return std::nullopt;
}

std::string service_state() {
    const auto r = systemctl({"is-active", unit_name()});
    return std::string(text::trim(r.output));
}

int report_started() {
    for (int i = 0; i < 25; ++i) {
        if (const auto status = query_daemon(); status && status->state != DaemonState::Starting) {
            std::cout << "keyboard-chatter-filter is running.\n" << describe_status(*status);
            return status->state == DaemonState::Error ? 1 : 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cerr << "error: the service did not report ready. Check: journalctl -u " << unit_name() << '\n';
    return 1;
}

bool remove_known_file(const std::filesystem::path& path, bool announce) {
    std::error_code ec;
    const bool existed = std::filesystem::exists(std::filesystem::symlink_status(path, ec));
    std::string error;
    if (!posix::remove_file_if_exists(path, error)) {
        std::cerr << "warning: " << error << '\n';
        return false;
    }
    if (existed && announce) {
        std::cout << "Removed " << path.string() << '\n';
    }
    return true;
}

}  // namespace
}  // namespace kcf::linux_input

namespace kcf::platform {

using namespace kcf::linux_input;

std::filesystem::path default_config_path() {
    return kConfigDir / "config.toml";
}

int run(const CliOptions& options) {
    DaemonOptions daemon_options;
    daemon_options.config_path = options.config_path.value_or(default_config_path());
    daemon_options.log_level_override = options.log_level;
    return run_daemon(daemon_options);
}

int install(const CliOptions&) {
    if (!require_root("install")) {
        return 1;
    }
    const auto exe = posix::current_executable();
    if (!exe) {
        std::cerr << "error: cannot determine the path of this executable\n";
        return 1;
    }
    std::string problem;
    if (!text::is_safe_unquoted_path(exe->string()) || !path_is_root_controlled(*exe, problem)) {
        std::cerr << "error: refusing to register " << exe->string() << " as a root service: "
                  << (problem.empty() ? "unsupported characters in path" : problem)
                  << ".\nInstall the binary to a root-owned location such as /usr/local/bin first.\n";
        return 1;
    }
    if (!systemctl_path()) {
        std::cerr << "error: systemd was not found. See docs/platform-linux.md for running the filter under "
                     "another init system.\n";
        return 1;
    }

    std::string error;
    if (!posix::ensure_directory(kConfigDir, 0755, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    const auto config = default_config_path();
    if (!std::filesystem::exists(config)) {
        if (!posix::write_file_atomic(config, default_configuration_text(), 0644, error)) {
            std::cerr << "error: " << error << '\n';
            return 1;
        }
        std::cout << "Created configuration file " << config.string() << '\n';
    }

    std::string unit = templates::k_linux_systemd_unit;
    unit = text::replace_all(std::move(unit), "{{EXECUTABLE}}", exe->string());
    unit = text::replace_all(std::move(unit), "{{SERVICE_ID}}", build::kServiceId);
    unit = text::replace_all(std::move(unit), "{{PROJECT_URL}}", build::kProjectUrl);
    if (!posix::write_file_atomic(unit_path(), unit, 0644, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    std::cout << "Installed systemd unit " << unit_path().string() << '\n';

    // uinput is usually loaded on demand, but make sure it is available at boot.
    if (std::filesystem::exists("/etc/modules-load.d") &&
        !posix::write_file_atomic(kModulesLoadFile, "# Needed by keyboard-chatter-filter\nuinput\n", 0644, error)) {
        std::cerr << "warning: " << error << '\n';
    }
    if (!std::filesystem::exists("/dev/uinput")) {
        if (const auto modprobe = posix::find_executable({"/usr/sbin/modprobe", "/sbin/modprobe"})) {
            posix::run_process({*modprobe, "uinput"});
        }
    }

    for (const std::vector<std::string>& args :
         {std::vector<std::string>{"daemon-reload"}, std::vector<std::string>{"enable", unit_name()},
          std::vector<std::string>{"restart", unit_name()}}) {
        const auto r = systemctl(args);
        if (r.exit_code != 0) {
            std::cerr << "error: systemctl " << args[0] << " failed: " << first_line(r.output) << '\n';
            return 1;
        }
    }
    std::cout << "Enabled " << unit_name() << " (starts at boot)\n";
    return report_started();
}

int start() {
    if (!std::filesystem::exists(unit_path())) {
        std::cerr << "error: the service is not installed. Run 'sudo keyboard-chatter-filter install' first.\n";
        return 1;
    }
    const auto r = systemctl({"start", unit_name()}, false);
    if (r.exit_code != 0) {
        std::cerr << "error: systemctl start failed" << (::geteuid() != 0 ? " (try with sudo)" : "") << '\n';
        return 1;
    }
    return report_started();
}

int stop() {
    const auto r = systemctl({"stop", unit_name()}, false);
    if (r.exit_code != 0) {
        std::cerr << "error: systemctl stop failed" << (::geteuid() != 0 ? " (try with sudo)" : "") << '\n';
        return 1;
    }
    std::cout << "Stopped. The keyboard is unfiltered until the next boot or 'keyboard-chatter-filter start'.\n";
    return 0;
}

int restart() {
    const auto r = systemctl({"restart", unit_name()}, false);
    if (r.exit_code != 0) {
        std::cerr << "error: systemctl restart failed" << (::geteuid() != 0 ? " (try with sudo)" : "") << '\n';
        return 1;
    }
    return report_started();
}

int reload() {
    if (service_state() == "active") {
        const auto r = systemctl({"reload", unit_name()}, false);
        if (r.exit_code != 0) {
            std::cerr << "error: systemctl reload failed" << (::geteuid() != 0 ? " (try with sudo)" : "") << '\n';
            return 1;
        }
        std::cout << "Configuration reload requested. Problems, if any, are logged: journalctl -u " << unit_name()
                  << '\n';
        return 0;
    }
    if (const auto pid = posix::InstanceLock::holder(runtime_paths_for_current_user().lock_file);
        pid && ::kill(*pid, SIGHUP) == 0) {
        std::cout << "Asked the running filter (pid " << *pid << ") to reload its configuration.\n";
        return 0;
    }
    std::cerr << "error: the filter is not running\n";
    return 1;
}

int status() {
    const bool installed = std::filesystem::exists(unit_path());
    if (const auto daemon = query_daemon()) {
        std::cout << "keyboard-chatter-filter is running.\n" << describe_status(*daemon);
        if (!installed) {
            std::cout << "  Autostart:   not installed\n";
        }
        std::cout << "  Log:         journalctl -u " << unit_name() << '\n';
        return daemon->state == DaemonState::Active || daemon->state == DaemonState::Disabled ? 0 : 1;
    }
    if (!installed) {
        std::cout << "keyboard-chatter-filter is not installed.\n";
        return 3;
    }
    const std::string state = service_state();
    std::cout << "keyboard-chatter-filter is installed but not running (systemd state: "
              << (state.empty() ? "unknown" : state) << ").\n  Start it:  sudo keyboard-chatter-filter start\n"
              << "  Log:       journalctl -u " << unit_name() << '\n';
    return 3;
}

// Executable named on the unit's ExecStart= line.
std::optional<std::filesystem::path> unit_executable() {
    std::ifstream in(unit_path());
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind("ExecStart=", 0) == 0) {
            const std::string value = line.substr(10);
            std::filesystem::path path(value.substr(0, value.find(' ')));
            if (path.is_absolute()) {
                return path;
            }
        }
    }
    return std::nullopt;
}

int uninstall(const CliOptions& options) {
    if (!require_root("uninstall")) {
        return 1;
    }
    int rc = 0;
    const auto installed_binary = unit_executable();
    if (systemctl_path()) {
        systemctl({"disable", "--now", unit_name()});
    }
    if (!remove_known_file(unit_path(), true)) {
        rc = 1;
    }
    remove_known_file(kModulesLoadFile, true);
    if (systemctl_path()) {
        systemctl({"daemon-reload"});
        systemctl({"reset-failed", unit_name()});
    }

    const auto config = default_config_path();
    if (options.purge) {
        remove_known_file(config, true);
        std::error_code ec;
        if (std::filesystem::is_directory(kConfigDir, ec) && std::filesystem::is_empty(kConfigDir, ec)) {
            std::filesystem::remove(kConfigDir, ec);
        }
    } else if (std::filesystem::exists(config)) {
        std::cout << "Kept " << config.string() << " (use --purge to remove it)\n";
    }

    // Remove the installed binary: the one the unit ran, or this one if it is a root-owned install.
    std::vector<std::filesystem::path> binaries;
    if (installed_binary) {
        binaries.push_back(*installed_binary);
    }
    if (const auto exe = posix::current_executable()) {
        binaries.push_back(*exe);
    }
    for (const auto& binary : binaries) {
        std::string problem;
        std::error_code ec;
        if (binary.filename() == build::kProgramName && std::filesystem::is_regular_file(binary, ec) &&
            path_is_root_controlled(binary, problem)) {
            remove_known_file(binary, true);
        }
    }
    std::cout << "keyboard-chatter-filter has been uninstalled.\n";
    return rc;
}

std::optional<int> run_platform_command(const CliOptions&) {
    return std::nullopt;
}

}  // namespace kcf::platform
