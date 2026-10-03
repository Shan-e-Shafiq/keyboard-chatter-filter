// SPDX-License-Identifier: MIT
#include "common/logging.h"

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <system_error>

#ifdef _WIN32
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace kcf::log {
namespace {

const char* journald_prefix(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "<3>";
        case LogLevel::Warning: return "<4>";
        case LogLevel::Info: return "<6>";
        case LogLevel::Debug: return "<7>";
    }
    return "<6>";
}

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000;
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char buffer[40];
    const std::size_t n = std::strftime(buffer, sizeof buffer, "%Y-%m-%d %H:%M:%S", &local);
    std::snprintf(buffer + n, sizeof buffer - n, ".%03d", static_cast<int>(millis));
    return buffer;
}

// Opens `path` for appending. On POSIX the file is created with mode 0600 and symlinks are refused.
std::FILE* open_append(const std::filesystem::path& path) {
#ifdef _WIN32
    std::FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"ab") != 0) {
        return nullptr;
    }
    return file;
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
        return nullptr;
    }
    std::FILE* file = ::fdopen(fd, "a");
    if (file == nullptr) {
        ::close(fd);
    }
    return file;
#endif
}

}  // namespace

Logger& logger() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    if (owns_file_ && file_ != nullptr) {
        std::fclose(file_);
    }
}

void Logger::use_stderr() {
    std::lock_guard lock(mutex_);
    if (owns_file_ && file_ != nullptr) {
        std::fclose(file_);
    }
    file_ = stderr;
    owns_file_ = false;
    max_bytes_ = 0;
    const char* journal = std::getenv("JOURNAL_STREAM");
    style_ = journal != nullptr && *journal != '\0' ? Style::Journald : Style::Timestamped;
}

bool Logger::use_file(const std::filesystem::path& path, std::uintmax_t max_bytes) {
    std::FILE* file = open_append(path);
    if (file == nullptr) {
        return false;
    }
    std::lock_guard lock(mutex_);
    if (owns_file_ && file_ != nullptr) {
        std::fclose(file_);
    }
    file_ = file;
    owns_file_ = true;
    path_ = path;
    max_bytes_ = max_bytes;
    style_ = Style::Timestamped;
    std::error_code ec;
    written_ = std::filesystem::file_size(path, ec);
    if (ec) {
        written_ = 0;
    }
    if (max_bytes_ != 0 && written_ > max_bytes_) {
        rotate_locked();
    }
    return true;
}

void Logger::rotate_locked() {
    if (!owns_file_ || file_ == nullptr) {
        return;
    }
    std::fclose(file_);
    file_ = nullptr;
    std::error_code ec;
    std::filesystem::path old = path_;
    old += ".1";
    std::filesystem::rename(path_, old, ec);
    if (ec) {
        // Could not move it aside: start over rather than growing without bound.
        std::filesystem::resize_file(path_, 0, ec);
    }
    file_ = open_append(path_);
    written_ = 0;
    if (file_ == nullptr) {
        file_ = stderr;
        owns_file_ = false;
    }
}

void Logger::write(LogLevel level, std::string_view message) {
    std::string line;
    line.reserve(message.size() + 48);
    if (style_ == Style::Journald) {
        line += journald_prefix(level);
    } else {
        line += timestamp_now();
        line += " [";
        line += to_string(level);
        line += "] ";
    }
    // Keep one record per line whatever the message contains.
    for (const char c : message) {
        line.push_back(c == '\n' || c == '\r' ? ' ' : c);
    }
    line.push_back('\n');

    std::lock_guard lock(mutex_);
    std::FILE* out = file_ != nullptr ? file_ : stderr;
    std::fwrite(line.data(), 1, line.size(), out);
    std::fflush(out);
    written_ += line.size();
    if (max_bytes_ != 0 && written_ > max_bytes_) {
        rotate_locked();
    }
}

}  // namespace kcf::log
