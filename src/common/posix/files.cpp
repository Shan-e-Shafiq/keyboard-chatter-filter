// SPDX-License-Identifier: MIT
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "common/posix/posix.h"

namespace kcf::posix {

bool ensure_directory(const std::filesystem::path& path, mode_t mode, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    if (ec) {
        error = "cannot create " + path.string() + ": " + ec.message();
        return false;
    }
    struct stat st {};
    if (::lstat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
        error = path.string() + " is not a directory";
        return false;
    }
    if ((st.st_mode & 07777) != mode && st.st_uid == ::geteuid() && ::chmod(path.c_str(), mode) != 0) {
        error = "cannot set permissions on " + path.string() + ": " + errno_message(errno);
        return false;
    }
    return true;
}

bool write_file_atomic(const std::filesystem::path& path, const std::string& content, mode_t mode,
                       std::string& error) {
    std::filesystem::path temp = path;
    temp += ".tmp-" + std::to_string(::getpid());
    UniqueFd fd(::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, mode));
    if (!fd) {
        error = "cannot create " + temp.string() + ": " + errno_message(errno);
        return false;
    }
    std::size_t done = 0;
    while (done < content.size()) {
        const ssize_t n = ::write(fd.get(), content.data() + done, content.size() - done);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            error = "cannot write " + temp.string() + ": " + errno_message(errno);
            ::unlink(temp.c_str());
            return false;
        }
        done += static_cast<std::size_t>(n);
    }
    // The umask may have narrowed the mode; apply it exactly.
    if (::fchmod(fd.get(), mode) != 0 || ::fsync(fd.get()) != 0) {
        error = "cannot finalise " + temp.string() + ": " + errno_message(errno);
        ::unlink(temp.c_str());
        return false;
    }
    fd.reset();
    if (::rename(temp.c_str(), path.c_str()) != 0) {
        error = "cannot replace " + path.string() + ": " + errno_message(errno);
        ::unlink(temp.c_str());
        return false;
    }
    return true;
}

bool remove_file_if_exists(const std::filesystem::path& path, std::string& error) {
    if (::unlink(path.c_str()) == 0 || errno == ENOENT) {
        return true;
    }
    error = "cannot remove " + path.string() + ": " + errno_message(errno);
    return false;
}

std::optional<std::filesystem::path> home_directory() {
    if (const char* home = std::getenv("HOME"); home != nullptr && home[0] == '/') {
        return std::filesystem::path(home);
    }
    long size = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    std::vector<char> buffer(size > 0 ? static_cast<std::size_t>(size) : 16384);
    passwd pw{};
    passwd* result = nullptr;
    if (::getpwuid_r(::getuid(), &pw, buffer.data(), buffer.size(), &result) == 0 && result != nullptr &&
        result->pw_dir != nullptr && result->pw_dir[0] == '/') {
        return std::filesystem::path(result->pw_dir);
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> current_executable() {
#ifdef __APPLE__
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(size + 1, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return std::nullopt;
    }
    char resolved[PATH_MAX];
    if (::realpath(buffer.data(), resolved) == nullptr) {
        return std::nullopt;
    }
    return std::filesystem::path(resolved);
#else
    std::error_code ec;
    auto path = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) {
        return std::nullopt;
    }
    return path;
#endif
}

}  // namespace kcf::posix
