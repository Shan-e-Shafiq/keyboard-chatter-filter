// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>

#include "keyboard_filter/configuration.h"

namespace kcf::macos {

struct DaemonOptions {
    std::filesystem::path config_path;
    std::optional<LogLevel> log_level_override;
    bool dry_run = false;  // observe and count only; never drop anything
};

// Runs the filter on the calling (main) thread's CFRunLoop until SIGTERM/SIGINT. SIGHUP reloads
// the configuration. Returns the process exit code.
int run_daemon(const DaemonOptions& options);

}  // namespace kcf::macos
