// SPDX-License-Identifier: MIT
#include "common/cli.h"

#include <charconv>

#include "kcf/build_info.h"

namespace kcf {
namespace {

struct CommandName {
    std::string_view name;
    Command command;
};

constexpr CommandName kCommands[] = {
    {"run", Command::Run},
    {"start", Command::Start},
    {"stop", Command::Stop},
    {"restart", Command::Restart},
    {"reload", Command::Reload},
    {"status", Command::Status},
    {"install", Command::Install},
    {"uninstall", Command::Uninstall},
    {"config", Command::Config},
    {"version", Command::Version},
    {"--version", Command::Version},
    {"-V", Command::Version},
    {"help", Command::Help},
    {"--help", Command::Help},
    {"-h", Command::Help},
    {"windows-service", Command::WindowsService},
    {"windows-agent", Command::WindowsAgent},
};

bool accepts_config(Command c) {
    return c == Command::Run || c == Command::Config || c == Command::WindowsAgent;
}

}  // namespace

CliParseResult parse_command_line(std::span<const std::string_view> args) {
    CliParseResult result;
    CliOptions options;
    if (args.empty()) {
        options.command = Command::Help;
        result.options = options;
        return result;
    }

    bool found = false;
    for (const CommandName& c : kCommands) {
        if (args[0] == c.name) {
            options.command = c.command;
            found = true;
            break;
        }
    }
    if (!found) {
        result.error = "unknown command '" + std::string(args[0]) + "'";
        return result;
    }

    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string_view arg = args[i];
        auto value_of = [&](std::string_view flag) -> std::optional<std::string_view> {
            if (arg.size() > flag.size() && arg.substr(0, flag.size()) == flag && arg[flag.size()] == '=') {
                return arg.substr(flag.size() + 1);
            }
            if (arg == flag && i + 1 < args.size()) {
                return args[++i];
            }
            return std::nullopt;
        };

        if (arg == "--config" || arg.starts_with("--config=")) {
            const auto value = value_of("--config");
            if (!accepts_config(options.command)) {
                result.error = "--config is not accepted by this command";
                return result;
            }
            if (!value || value->empty()) {
                result.error = "--config requires a path";
                return result;
            }
            options.config_path = std::filesystem::path(std::string(*value));
        } else if (arg == "--log-level" || arg.starts_with("--log-level=")) {
            const auto value = value_of("--log-level");
            if (options.command != Command::Run && options.command != Command::WindowsAgent) {
                result.error = "--log-level is only accepted by 'run'";
                return result;
            }
            const auto level = value ? parse_log_level(*value) : std::nullopt;
            if (!level) {
                result.error = "--log-level requires one of: error, warning, info, debug";
                return result;
            }
            options.log_level = level;
        } else if (arg == "--dry-run") {
            if (options.command != Command::Run) {
                result.error = "--dry-run is only accepted by 'run'";
                return result;
            }
            options.dry_run = true;
        } else if (arg == "--purge") {
            if (options.command != Command::Uninstall) {
                result.error = "--purge is only accepted by 'uninstall'";
                return result;
            }
            options.purge = true;
        } else if (arg == "--stop-event" || arg.starts_with("--stop-event=") || arg == "--reload-event" ||
                   arg.starts_with("--reload-event=")) {
            const bool stop = arg.starts_with("--stop-event");
            const auto value = value_of(stop ? "--stop-event" : "--reload-event");
            std::uint64_t handle = 0;
            if (options.command != Command::WindowsAgent || !value ||
                std::from_chars(value->data(), value->data() + value->size(), handle).ec != std::errc()) {
                result.error = stop ? "invalid --stop-event" : "invalid --reload-event";
                return result;
            }
            (stop ? options.stop_event : options.reload_event) = handle;
        } else {
            result.error = "unexpected argument '" + std::string(arg) + "'";
            return result;
        }
    }
    result.options = options;
    return result;
}

std::string usage_text() {
    std::string text = "keyboard-chatter-filter ";
    text += build::kVersion;
    text += R"( - removes duplicate keystrokes caused by keyboard switch chatter

Usage: keyboard-chatter-filter <command> [options]

Service commands:
  status              Show whether the filter is running and what it is doing
  start               Start the background service (it also starts automatically at login/boot)
  stop                Stop the background service until the next login/boot or `start`
  restart             Restart the background service
  reload              Re-read the configuration file without restarting
  install             Register the background service for this binary (done by the installer)
  uninstall [--purge] Stop and remove the service and this binary; --purge also removes
                      the configuration file and logs

Other commands:
  config [--config PATH]   Show the configuration file in use, effective settings and problems
  run [--config PATH] [--log-level LEVEL] [--dry-run]
                           Run the filter in the foreground (the service runs this).
                           --dry-run only observes and counts what it would remove
  version                  Print the version
  help                     Show this help

Keystrokes are processed in memory only. Nothing you type is logged, stored or sent anywhere.
)";
    return text;
}

}  // namespace kcf
