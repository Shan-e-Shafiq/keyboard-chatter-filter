// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "keyboard_filter/configuration.h"

namespace kcf {

enum class Command {
    Run,        // filter in the foreground (what the service manager runs)
    Start,
    Stop,
    Restart,
    Reload,
    Status,
    Install,    // register the background service for the installed binary
    Uninstall,
    Config,     // show the configuration file location, effective values and problems
    Version,
    Help,
    // Windows internals, launched by the service control manager and the service respectively.
    WindowsService,
    WindowsAgent,
};

struct CliOptions {
    Command command = Command::Help;
    std::optional<std::filesystem::path> config_path;  // --config
    std::optional<LogLevel> log_level;                 // --log-level (run)
    bool purge = false;                                // --purge (uninstall)
    bool dry_run = false;                              // --dry-run (run): observe only, never drop
    std::optional<std::uint64_t> stop_event;           // --stop-event (Windows agent, inherited handle value)
    std::optional<std::uint64_t> reload_event;         // --reload-event (Windows agent, inherited handle value)
};

struct CliParseResult {
    std::optional<CliOptions> options;
    std::string error;
};

[[nodiscard]] CliParseResult parse_command_line(std::span<const std::string_view> args);
[[nodiscard]] std::string usage_text();

}  // namespace kcf
