// SPDX-License-Identifier: MIT
//
// Per-user LaunchAgent management through launchctl (bootstrap/bootout/kickstart). launchctl is
// executed directly with an argument vector; nothing goes through a shell.
#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace kcf::macos {

[[nodiscard]] std::string launch_agent_label();

[[nodiscard]] std::string render_launch_agent(const std::filesystem::path& executable,
                                              const std::filesystem::path& log_file);

// Executable path recorded in an installed LaunchAgent plist.
[[nodiscard]] std::optional<std::filesystem::path> launch_agent_executable(const std::filesystem::path& plist);

struct LaunchdJob {
    bool loaded = false;
    std::optional<long> pid;  // set while the process is running
};

[[nodiscard]] LaunchdJob query_job();

// Each returns true on success; on failure `error` describes what launchctl said.
bool bootstrap(const std::filesystem::path& plist, std::string& error);
bool bootout(std::string& error);  // succeeds if the job was not loaded
bool kickstart(bool kill_running, std::string& error);
bool send_signal(const char* signal_name, std::string& error);

}  // namespace kcf::macos
