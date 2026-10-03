// SPDX-License-Identifier: MIT
#include <poll.h>
#include <random>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

#include "common/posix/posix.h"
#include "test_framework.h"

using namespace kcf;

namespace {

class TempDir {
public:
    TempDir() {
        std::random_device rd;
        // Short path: Unix socket paths are limited to ~104 bytes.
        path_ = std::filesystem::path("/tmp") / ("kcf-" + std::to_string(rd() % 1000000));
        std::filesystem::create_directories(path_);
    }
    ~TempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    [[nodiscard]] const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

mode_t mode_of(const std::filesystem::path& p) {
    struct stat st {};
    ::lstat(p.c_str(), &st);
    return st.st_mode & 07777;
}

}  // namespace

TEST(Posix, InstanceLockIsExclusive) {
    TempDir dir;
    const auto path = dir.path() / "daemon.lock";
    std::string error;
    auto first = posix::InstanceLock::acquire(path, error);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(mode_of(path), 0600u);

    const auto holder = posix::InstanceLock::holder(path);
    ASSERT_TRUE(holder.has_value());
    EXPECT_EQ(*holder, ::getpid());

    auto second = posix::InstanceLock::acquire(path, error);
    EXPECT_FALSE(second.has_value());
    EXPECT_NE(error.find("already running"), std::string::npos);

    first.reset();
    EXPECT_FALSE(posix::InstanceLock::holder(path).has_value());
    auto third = posix::InstanceLock::acquire(path, error);
    EXPECT_TRUE(third.has_value());
}

TEST(Posix, StatusSocketRoundTrip) {
    TempDir dir;
    const auto path = dir.path() / "s.sock";
    posix::StatusServer server;
    std::string error;
    ASSERT_TRUE(server.open(path, 0600, error));
    EXPECT_EQ(mode_of(path), 0600u);

    std::optional<std::string> answer;
    std::thread client([&] { answer = posix::query_status_socket(path, std::chrono::milliseconds(3000)); });
    for (int i = 0; i < 300 && !answer; ++i) {
        pollfd pfd{server.fd(), POLLIN, 0};
        if (::poll(&pfd, 1, 10) > 0) {
            server.serve_pending([] { return std::string("state=active\n"); });
        }
    }
    client.join();
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(*answer, "state=active\n");

    server.close();
    EXPECT_FALSE(std::filesystem::exists(path));
    EXPECT_FALSE(posix::query_status_socket(path, std::chrono::milliseconds(100)).has_value());
}

TEST(Posix, StatusSocketRefusesToReplaceRegularFile) {
    TempDir dir;
    const auto path = dir.path() / "s.sock";
    std::string error;
    ASSERT_TRUE(posix::write_file_atomic(path, "data", 0600, error));
    posix::StatusServer server;
    EXPECT_FALSE(server.open(path, 0600, error));
    EXPECT_TRUE(std::filesystem::exists(path));
}

TEST(Posix, RunProcessCapturesOutputWithoutShell) {
    const auto echo = posix::find_executable({"/bin/echo", "/usr/bin/echo"});
    ASSERT_TRUE(echo.has_value());
    // Shell metacharacters are passed literally: nothing is interpreted.
    const auto r = posix::run_process({*echo, "hello; rm -rf / $(id)"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "hello; rm -rf / $(id)\n");

    const auto f = posix::find_executable({"/usr/bin/false", "/bin/false"});
    ASSERT_TRUE(f.has_value());
    EXPECT_EQ(posix::run_process({*f}).exit_code, 1);
}

TEST(Posix, RunProcessRequiresAbsolutePath) {
    const auto r = posix::run_process({"echo", "x"});
    EXPECT_EQ(r.exit_code, -1);
    EXPECT_EQ(posix::run_process({}).exit_code, -1);
}

TEST(Posix, AtomicWriteAppliesMode) {
    TempDir dir;
    const auto path = dir.path() / "file.txt";
    std::string error;
    ASSERT_TRUE(posix::write_file_atomic(path, "one", 0644, error));
    EXPECT_EQ(mode_of(path), 0644u);
    ASSERT_TRUE(posix::write_file_atomic(path, "two", 0600, error));
    EXPECT_EQ(mode_of(path), 0600u);
}

TEST(Posix, EnsureDirectoryAndRemove) {
    TempDir dir;
    const auto nested = dir.path() / "a" / "b";
    std::string error;
    ASSERT_TRUE(posix::ensure_directory(nested, 0700, error));
    EXPECT_EQ(mode_of(nested), 0700u);
    EXPECT_TRUE(posix::remove_file_if_exists(nested / "missing", error));  // idempotent
}

TEST(Posix, CurrentExecutableAndHome) {
    const auto exe = posix::current_executable();
    ASSERT_TRUE(exe.has_value());
    EXPECT_TRUE(exe->is_absolute());
    EXPECT_TRUE(std::filesystem::exists(*exe));
    EXPECT_TRUE(posix::home_directory().has_value());
}

KCF_TEST_MAIN()
