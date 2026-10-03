// SPDX-License-Identifier: MIT
//
// Snapshot of the running daemon, exchanged with the CLI as "key=value" lines. Deliberately limited
// to operational facts: nothing about which keys were pressed is ever exposed.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace kcf {

enum class DaemonState : std::uint8_t {
    Starting,
    Active,                // intercepting and filtering
    WaitingForPermission,  // the OS has not granted the access interception requires
    Disabled,              // running, but `enabled = false` in the configuration
    Error,                 // interception could not be set up; retrying
};

[[nodiscard]] const char* to_string(DaemonState state) noexcept;
[[nodiscard]] std::optional<DaemonState> parse_daemon_state(std::string_view text) noexcept;

struct DaemonStatus {
    std::string version;
    std::int64_t pid = 0;
    DaemonState state = DaemonState::Starting;
    std::string detail;  // one line of human-readable explanation
    std::int64_t threshold_ms = 0;
    bool enabled = true;
    std::int64_t devices = -1;  // keyboards being filtered; -1 when the platform cannot tell
    std::uint64_t suppressed = 0;  // chatter keystrokes removed since start
    std::string config_path;
    std::int64_t config_problems = 0;
    std::int64_t uptime_seconds = 0;
};

[[nodiscard]] std::string serialize_status(const DaemonStatus& status);
[[nodiscard]] std::optional<DaemonStatus> parse_status(std::string_view text);

// Multi-line, human-readable rendering for `keyboard-chatter-filter status`.
[[nodiscard]] std::string describe_status(const DaemonStatus& status);

}  // namespace kcf
