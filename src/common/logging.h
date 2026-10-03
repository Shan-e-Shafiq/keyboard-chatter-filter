// SPDX-License-Identifier: MIT
//
// Minimal levelled logger. Messages describe the daemon's lifecycle and errors only: callers must
// never pass key codes, characters or anything else derived from what the user types.
#pragma once

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>

#include "keyboard_filter/configuration.h"

namespace kcf::log {

class Logger {
public:
    enum class Style {
        Timestamped,  // "2026-10-03 14:02:11.123 [info] message"
        Journald,     // "<6>message" - systemd assigns priority and timestamp
    };

    Logger() = default;
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    void set_level(LogLevel level) noexcept { level_ = level; }
    [[nodiscard]] LogLevel level() const noexcept { return level_; }
    [[nodiscard]] bool enabled(LogLevel level) const noexcept {
        return static_cast<int>(level) <= static_cast<int>(level_);
    }

    // Writes to stderr. Picks journald style automatically when running under systemd.
    void use_stderr();

    // Appends to `path`, creating it (owner read/write only on POSIX). When the file grows beyond
    // `max_bytes` it is moved to "<path>.1" (replacing an older one) and a fresh file is started.
    bool use_file(const std::filesystem::path& path, std::uintmax_t max_bytes = 1024 * 1024);

    void write(LogLevel level, std::string_view message);

private:
    void rotate_locked();

    std::mutex mutex_;
    std::FILE* file_ = nullptr;
    bool owns_file_ = false;
    std::filesystem::path path_;
    std::uintmax_t max_bytes_ = 0;
    std::uintmax_t written_ = 0;
    Style style_ = Style::Timestamped;
    LogLevel level_ = LogLevel::Info;
};

// Process-wide logger. Configured once at startup by main().
Logger& logger();

template <typename... Args>
std::string concat(const Args&... args) {
    std::ostringstream out;
    (out << ... << args);
    return out.str();
}

template <typename... Args>
void error(const Args&... args) {
    if (logger().enabled(LogLevel::Error)) {
        logger().write(LogLevel::Error, concat(args...));
    }
}

template <typename... Args>
void warning(const Args&... args) {
    if (logger().enabled(LogLevel::Warning)) {
        logger().write(LogLevel::Warning, concat(args...));
    }
}

template <typename... Args>
void info(const Args&... args) {
    if (logger().enabled(LogLevel::Info)) {
        logger().write(LogLevel::Info, concat(args...));
    }
}

template <typename... Args>
void debug(const Args&... args) {
    if (logger().enabled(LogLevel::Debug)) {
        logger().write(LogLevel::Debug, concat(args...));
    }
}

}  // namespace kcf::log
