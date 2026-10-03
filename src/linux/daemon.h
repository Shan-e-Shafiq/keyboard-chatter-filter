// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "keyboard_filter/configuration.h"

namespace kcf::linux_input {

struct DaemonOptions {
    std::filesystem::path config_path;
    std::optional<LogLevel> log_level_override;
    // Test hook: only consider input devices with exactly this name (never set by the CLI).
    std::optional<std::string> only_device_name;
    // Test hook: where to look for evdev nodes.
    std::filesystem::path input_dir = "/dev/input";
};

struct RuntimePaths {
    std::filesystem::path dir;            // /run/keyboard-chatter-filter when running as root
    std::filesystem::path lock_file;
    std::filesystem::path status_socket;
    bool system_wide = false;
};

// Runtime paths for a daemon running as the current user (root: the systemd RuntimeDirectory).
[[nodiscard]] RuntimePaths runtime_paths_for_current_user();
// Where the system service publishes its status socket.
[[nodiscard]] RuntimePaths system_runtime_paths();

// Runs until SIGTERM/SIGINT; SIGHUP reloads the configuration. Returns the process exit code.
int run_daemon(const DaemonOptions& options);

}  // namespace kcf::linux_input
