// SPDX-License-Identifier: MIT
#include <cerrno>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "common/posix/posix.h"

extern char** environ;

namespace kcf::posix {

ProcessResult run_process(const std::vector<std::string>& argv, bool capture_output) {
    ProcessResult result;
    if (argv.empty() || argv[0].empty() || argv[0][0] != '/') {
        result.output = "internal error: refusing to run a command without an absolute path";
        return result;
    }

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& a : argv) {
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);

    int pipe_fds[2] = {-1, -1};
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (capture_output) {
        if (::pipe(pipe_fds) != 0) {
            posix_spawn_file_actions_destroy(&actions);
            result.output = "pipe: " + errno_message(errno);
            return result;
        }
        ::fcntl(pipe_fds[0], F_SETFD, FD_CLOEXEC);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDERR_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[1]);
    }
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

    pid_t pid = 0;
    const int rc = ::posix_spawn(&pid, args[0], &actions, nullptr, args.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (capture_output) {
        ::close(pipe_fds[1]);
    }
    if (rc != 0) {
        if (capture_output) {
            ::close(pipe_fds[0]);
        }
        result.output = "cannot run " + argv[0] + ": " + errno_message(rc);
        return result;
    }

    if (capture_output) {
        char buffer[4096];
        while (true) {
            const ssize_t n = ::read(pipe_fds[0], buffer, sizeof buffer);
            if (n > 0) {
                if (result.output.size() < 1024 * 1024) {
                    result.output.append(buffer, static_cast<std::size_t>(n));
                }
            } else if (n == 0 || errno != EINTR) {
                break;
            }
        }
        ::close(pipe_fds[0]);
    }

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            return result;
        }
    }
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}

std::optional<std::string> find_executable(std::initializer_list<const char*> candidates) {
    for (const char* candidate : candidates) {
        if (::access(candidate, X_OK) == 0) {
            return std::string(candidate);
        }
    }
    return std::nullopt;
}

}  // namespace kcf::posix
