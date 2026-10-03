// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "keyboard_filter/chatter_filter.h"

namespace kcf {

enum class LogLevel : std::uint8_t { Error, Warning, Info, Debug };

[[nodiscard]] const char* to_string(LogLevel level) noexcept;
[[nodiscard]] std::optional<LogLevel> parse_log_level(std::string_view text) noexcept;

struct Configuration {
    static constexpr std::chrono::milliseconds kDefaultThreshold{30};
    static constexpr std::chrono::milliseconds kMinThreshold{1};
    static constexpr std::chrono::milliseconds kMaxThreshold{200};
    // Above this, intentional fast double letters start to get merged; we warn but allow it.
    static constexpr std::chrono::milliseconds kHighThresholdWarning{80};

    std::chrono::milliseconds chatter_threshold = kDefaultThreshold;
    bool enabled = true;
    LogLevel log_level = LogLevel::Info;
    // Linux only: input devices whose name contains one of these strings (case-insensitive) are
    // never intercepted.
    std::vector<std::string> ignored_devices;

    [[nodiscard]] FilterSettings filter_settings() const noexcept {
        return FilterSettings{chatter_threshold, enabled};
    }

    friend bool operator==(const Configuration&, const Configuration&) = default;
};

struct ConfigDiagnostic {
    enum class Severity : std::uint8_t { Warning, Error };

    Severity severity = Severity::Error;
    std::size_t line = 0;  // 1-based; 0 = not tied to a line
    std::string message;
};

struct ConfigLoadResult {
    Configuration config;  // always usable: invalid or missing values fall back to defaults
    std::vector<ConfigDiagnostic> diagnostics;
    bool file_found = false;

    [[nodiscard]] bool has_errors() const noexcept;
    // Human-readable "path:line: severity: message" lines.
    [[nodiscard]] std::vector<std::string> describe(const std::filesystem::path& path) const;
};

// Configuration files are a strict subset of TOML: `key = value` lines with integers, booleans,
// strings and single-line arrays of strings, plus `#` comments.
inline constexpr std::size_t kMaxConfigFileBytes = 64 * 1024;

[[nodiscard]] ConfigLoadResult parse_configuration(std::string_view text);

// A missing file is not an error (defaults apply). Unreadable or oversized files are reported and
// defaults apply.
[[nodiscard]] ConfigLoadResult load_configuration(const std::filesystem::path& path);

// Commented configuration file written on installation.
[[nodiscard]] std::string default_configuration_text();

}  // namespace kcf
