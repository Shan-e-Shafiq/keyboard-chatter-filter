// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <unistd.h>

#include "common/posix/posix.h"

namespace kcf::posix {

void UniqueFd::reset(int fd) noexcept {
    if (fd_ >= 0) {
        ::close(fd_);
    }
    fd_ = fd;
}

std::string errno_message(int error) {
    return std::strerror(error);
}

std::optional<InstanceLock> InstanceLock::acquire(const std::filesystem::path& path, std::string& error) {
    UniqueFd fd(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600));
    if (!fd) {
        error = "cannot open lock file " + path.string() + ": " + errno_message(errno);
        return std::nullopt;
    }
    if (::flock(fd.get(), LOCK_EX | LOCK_NB) != 0) {
        const int err = errno;
        if (err == EWOULDBLOCK) {
            const auto pid = holder(path);
            error = "another instance is already running" + (pid ? " (pid " + std::to_string(*pid) + ")" : std::string());
        } else {
            error = "cannot lock " + path.string() + ": " + errno_message(err);
        }
        return std::nullopt;
    }
    const std::string pid = std::to_string(::getpid()) + "\n";
    if (::ftruncate(fd.get(), 0) == 0) {
        [[maybe_unused]] const auto written = ::pwrite(fd.get(), pid.data(), pid.size(), 0);
    }
    return InstanceLock(std::move(fd));
}

std::optional<pid_t> InstanceLock::holder(const std::filesystem::path& path) {
    UniqueFd fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW));
    if (!fd) {
        return std::nullopt;
    }
    if (::flock(fd.get(), LOCK_SH | LOCK_NB) == 0) {
        ::flock(fd.get(), LOCK_UN);
        return std::nullopt;  // nobody holds it
    }
    char buffer[32] = {};
    const ssize_t n = ::pread(fd.get(), buffer, sizeof buffer - 1, 0);
    if (n <= 0) {
        return std::nullopt;
    }
    const long pid = std::strtol(buffer, nullptr, 10);
    if (pid <= 0) {
        return std::nullopt;
    }
    return static_cast<pid_t>(pid);
}

}  // namespace kcf::posix
