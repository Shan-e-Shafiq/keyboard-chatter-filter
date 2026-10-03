// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>

#include "keyboard_filter/configuration.h"

namespace kcf::windows {

struct AgentOptions {
    std::filesystem::path config_path;
    std::optional<LogLevel> log_level_override;
    HANDLE stop_event = nullptr;    // signalled by the service to stop (inherited); console Ctrl+C otherwise
    HANDLE reload_event = nullptr;  // signalled by the service to reload the configuration (inherited)
    bool dry_run = false;           // observe and count only; never block anything
};

// Exit codes the service supervisor understands.
inline constexpr int kAgentExitOk = 0;
inline constexpr int kAgentExitFailure = 1;
inline constexpr int kAgentExitAlreadyRunning = 3;

// Runs the keyboard hook in the current (interactive) session until stopped.
int run_agent(const AgentOptions& options);

}  // namespace kcf::windows
