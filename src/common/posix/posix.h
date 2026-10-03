// SPDX-License-Identifier: MIT
//
// Small RAII wrappers and helpers shared by the macOS and Linux builds.
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <sys/types.h>
#include <vector>

namespace kcf::posix {

// Owning file descriptor.
class UniqueFd {
public:
    UniqueFd() noexcept = default;
    explicit UniqueFd(int fd) noexcept : fd_(fd) {}
    ~UniqueFd() { reset(); }
    UniqueFd(UniqueFd&& other) noexcept : fd_(other.release()) {}
    UniqueFd& operator=(UniqueFd&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }
    UniqueFd(const UniqueFd&) = delete;
    UniqueFd& operator=(const UniqueFd&) = delete;

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] bool valid() const noexcept { return fd_ >= 0; }
    explicit operator bool() const noexcept { return valid(); }
    int release() noexcept {
        const int fd = fd_;
        fd_ = -1;
        return fd;
    }
    void reset(int fd = -1) noexcept;

private:
    int fd_ = -1;
};

[[nodiscard]] std::string errno_message(int error);

// Holds an exclusive advisory lock (flock) on a file for the lifetime of the object, so a second
// daemon instance can detect the first. The lock disappears automatically if the process dies.
class InstanceLock {
public:
    // Returns std::nullopt and sets `error` if another process holds the lock or the file cannot
    // be opened. On success the file contains the current PID.
    static std::optional<InstanceLock> acquire(const std::filesystem::path& path, std::string& error);

    // PID recorded by the current holder of the lock, if any process holds it.
    static std::optional<pid_t> holder(const std::filesystem::path& path);

private:
    explicit InstanceLock(UniqueFd fd) : fd_(std::move(fd)) {}
    UniqueFd fd_;
};

struct ProcessResult {
    int exit_code = -1;  // -1 if the process could not be started or was killed by a signal
    std::string output;  // stdout and stderr, when captured
};

// Runs `argv` directly (no shell, so no quoting or injection issues) and waits for it.
// argv[0] must be an absolute path.
ProcessResult run_process(const std::vector<std::string>& argv, bool capture_output = true);

// Returns the first of `candidates` that is an executable file.
[[nodiscard]] std::optional<std::string> find_executable(std::initializer_list<const char*> candidates);

// Creates `path` (and parents) and applies `mode` to the leaf directory.
bool ensure_directory(const std::filesystem::path& path, mode_t mode, std::string& error);

// Atomically replaces `path` with `content` (temporary file + rename) using `mode`.
bool write_file_atomic(const std::filesystem::path& path, const std::string& content, mode_t mode,
                       std::string& error);

// Removes a regular file or symlink if it exists. Returns false only on a real failure.
bool remove_file_if_exists(const std::filesystem::path& path, std::string& error);

[[nodiscard]] std::optional<std::filesystem::path> home_directory();

// Absolute path of the running executable, symlinks resolved.
[[nodiscard]] std::optional<std::filesystem::path> current_executable();

// Unix-domain socket that answers every connection with one snapshot and closes it. The server
// never reads from clients, so a misbehaving client can never block the daemon.
class StatusServer {
public:
    StatusServer() = default;
    ~StatusServer();
    StatusServer(const StatusServer&) = delete;
    StatusServer& operator=(const StatusServer&) = delete;

    bool open(const std::filesystem::path& path, mode_t mode, std::string& error);
    void close();
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    // Call when fd() is readable: accepts every pending connection and answers each one.
    void serve_pending(const std::function<std::string()>& snapshot);

private:
    UniqueFd fd_;
    std::filesystem::path path_;
};

// Reads the snapshot published by a StatusServer. std::nullopt if nothing is listening.
[[nodiscard]] std::optional<std::string> query_status_socket(const std::filesystem::path& path,
                                                             std::chrono::milliseconds timeout);

}  // namespace kcf::posix
