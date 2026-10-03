// SPDX-License-Identifier: MIT
#pragma once

#include <filesystem>
#include <optional>

namespace kcf::macos {

struct Paths {
    std::filesystem::path config_file;   // ~/.config/keyboard-chatter-filter/config.toml
    std::filesystem::path state_dir;     // ~/Library/Application Support/keyboard-chatter-filter
    std::filesystem::path lock_file;     // <state_dir>/daemon.lock
    std::filesystem::path status_socket; // <state_dir>/status.sock
    std::filesystem::path log_dir;       // ~/Library/Logs/keyboard-chatter-filter
    std::filesystem::path log_file;      // <log_dir>/keyboard-chatter-filter.log
    std::filesystem::path launch_agent;  // ~/Library/LaunchAgents/<label>.plist
};

// std::nullopt if the home directory cannot be determined.
[[nodiscard]] std::optional<Paths> user_paths();

}  // namespace kcf::macos
