// SPDX-License-Identifier: MIT
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/posix/posix.h"

namespace kcf::posix {
namespace {

bool make_address(const std::filesystem::path& path, sockaddr_un& addr, std::string& error) {
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    const std::string p = path.string();
    if (p.size() >= sizeof addr.sun_path) {
        error = "socket path too long: " + p;
        return false;
    }
    std::memcpy(addr.sun_path, p.c_str(), p.size() + 1);
    return true;
}

void set_cloexec_nonblock(int fd) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
}

void disable_sigpipe([[maybe_unused]] int fd) {
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif

}  // namespace

StatusServer::~StatusServer() {
    close();
}

bool StatusServer::open(const std::filesystem::path& path, mode_t mode, std::string& error) {
    close();
    sockaddr_un addr{};
    if (!make_address(path, addr, error)) {
        return false;
    }
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd) {
        error = "socket: " + errno_message(errno);
        return false;
    }
    set_cloexec_nonblock(fd.get());

    // A stale socket from a previous run (the instance lock guarantees it is not in use).
    struct stat st {};
    if (::lstat(path.c_str(), &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            error = path.string() + " exists and is not a socket";
            return false;
        }
        ::unlink(path.c_str());
    }

    const mode_t old_mask = ::umask(static_cast<mode_t>(~mode & 0777));
    const int rc = ::bind(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
    ::umask(old_mask);
    if (rc != 0) {
        error = "bind " + path.string() + ": " + errno_message(errno);
        return false;
    }
    ::chmod(path.c_str(), mode);
    if (::listen(fd.get(), 8) != 0) {
        error = "listen: " + errno_message(errno);
        ::unlink(path.c_str());
        return false;
    }
    fd_ = std::move(fd);
    path_ = path;
    return true;
}

void StatusServer::close() {
    if (fd_) {
        fd_.reset();
        ::unlink(path_.c_str());
    }
}

void StatusServer::serve_pending(const std::function<std::string()>& snapshot) {
    if (!fd_) {
        return;
    }
    std::string payload;
    for (int i = 0; i < 16; ++i) {  // bounded: never starve keyboard handling
        UniqueFd client(::accept(fd_.get(), nullptr, nullptr));
        if (!client) {
            return;  // EAGAIN or a transient error
        }
        set_cloexec_nonblock(client.get());
        disable_sigpipe(client.get());
        if (payload.empty()) {
            payload = snapshot();
        }
        // The payload is far smaller than a socket buffer, so this cannot block; if a client
        // somehow cannot take it, it simply gets a truncated answer.
        [[maybe_unused]] const ssize_t sent = ::send(client.get(), payload.data(), payload.size(), kSendFlags);
    }
}

std::optional<std::string> query_status_socket(const std::filesystem::path& path,
                                               std::chrono::milliseconds timeout) {
    sockaddr_un addr{};
    std::string error;
    if (!make_address(path, addr, error)) {
        return std::nullopt;
    }
    UniqueFd fd(::socket(AF_UNIX, SOCK_STREAM, 0));
    if (!fd) {
        return std::nullopt;
    }
    ::fcntl(fd.get(), F_SETFD, FD_CLOEXEC);
    disable_sigpipe(fd.get());
    if (::connect(fd.get(), reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
        return std::nullopt;
    }
    std::string out;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (out.size() < 64 * 1024) {
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            return std::nullopt;
        }
        pollfd pfd{fd.get(), POLLIN, 0};
        const int ready = ::poll(&pfd, 1, static_cast<int>(remaining.count()));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            return std::nullopt;
        }
        char buffer[1024];
        const ssize_t n = ::read(fd.get(), buffer, sizeof buffer);
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return std::nullopt;
        }
        out.append(buffer, static_cast<std::size_t>(n));
    }
    return out;
}

}  // namespace kcf::posix
