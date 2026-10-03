// SPDX-License-Identifier: MIT
//
// End-to-end test of the Linux adapter against the real kernel input stack:
//
//   synthetic "physical" keyboard (uinput) -> evdev -> daemon (grab + filter) -> virtual keyboard
//
// The daemon runs in a child process restricted to the synthetic device, so a developer's real
// keyboards are never touched. Requires read/write access to /dev/uinput and /dev/input/event*
// (root, or CI with `sudo`); otherwise exits with 77, which CTest reports as skipped.

#include <linux/input.h>
#include <linux/uinput.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "common/logging.h"
#include "linux/daemon.h"
#include "linux/device_classifier.h"
#include "linux/input_device.h"
#include "test_framework.h"

using namespace std::chrono_literals;

namespace {

struct KeyRecord {
    unsigned code;
    int value;
    friend bool operator==(const KeyRecord&, const KeyRecord&) = default;
};

std::ostream& operator<<(std::ostream& os, const std::vector<KeyRecord>& records) {
    for (const auto& r : records) {
        os << r.code << ':' << r.value << ' ';
    }
    return os;
}

void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

std::string device_name(const std::filesystem::path& node) {
    const int fd = ::open(node.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return {};
    }
    char name[256] = {};
    kcf::linux_input::device_ioctl(fd, EVIOCGNAME(sizeof name - 1), name);
    ::close(fd);
    return name;
}

std::optional<std::filesystem::path> find_node(const std::string& name, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator("/dev/input", ec)) {
            if (entry.path().filename().string().rfind("event", 0) == 0 && device_name(entry.path()) == name) {
                return entry.path();
            }
        }
        sleep_ms(20);
    }
    return std::nullopt;
}

// The synthetic keyboard the test types on.
class FakeKeyboard {
public:
    explicit FakeKeyboard(const std::string& name) {
        fd_ = ::open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd_ < 0) {
            return;
        }
        kcf::linux_input::device_ioctl(fd_, UI_SET_EVBIT, EV_KEY);
        kcf::linux_input::device_ioctl(fd_, UI_SET_EVBIT, EV_REP);  // kernel auto-repeat, like a real keyboard
        for (int k = KEY_ESC; k <= KEY_KPDOT; ++k) {
            kcf::linux_input::device_ioctl(fd_, UI_SET_KEYBIT, k);
        }
        uinput_setup setup{};
        setup.id.bustype = BUS_USB;
        setup.id.vendor = 0x1234;
        setup.id.product = 0x5678;
        std::strncpy(setup.name, name.c_str(), UINPUT_MAX_NAME_SIZE - 1);
        if (kcf::linux_input::device_ioctl(fd_, UI_DEV_SETUP, &setup) != 0 || kcf::linux_input::device_ioctl(fd_, UI_DEV_CREATE) != 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }
    ~FakeKeyboard() {
        if (fd_ >= 0) {
            kcf::linux_input::device_ioctl(fd_, UI_DEV_DESTROY);
            ::close(fd_);
        }
    }
    FakeKeyboard(const FakeKeyboard&) = delete;
    FakeKeyboard& operator=(const FakeKeyboard&) = delete;

    [[nodiscard]] bool ok() const { return fd_ >= 0; }

    void key(unsigned code, int value) {
        input_event ev[2]{};
        ev[0].type = EV_KEY;
        ev[0].code = static_cast<std::uint16_t>(code);
        ev[0].value = value;
        ev[1].type = EV_SYN;
        ev[1].code = SYN_REPORT;
        [[maybe_unused]] const auto n = ::write(fd_, ev, sizeof ev);
    }

private:
    int fd_ = -1;
};

// Reads key events applications receive from the filtered virtual keyboard.
std::vector<KeyRecord> collect(int fd, std::chrono::milliseconds quiet_for) {
    std::vector<KeyRecord> out;
    auto last = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - last < quiet_for) {
        input_event ev{};
        const ssize_t n = ::read(fd, &ev, sizeof ev);
        if (n == static_cast<ssize_t>(sizeof ev)) {
            if (ev.type == EV_KEY) {
                out.push_back({ev.code, ev.value});
            }
            last = std::chrono::steady_clock::now();
        } else {
            sleep_ms(2);
        }
    }
    return out;
}

std::vector<KeyRecord> without_repeats(const std::vector<KeyRecord>& in) {
    std::vector<KeyRecord> out;
    std::copy_if(in.begin(), in.end(), std::back_inserter(out), [](const KeyRecord& r) { return r.value != 2; });
    return out;
}

pid_t start_daemon(const std::string& device, const std::filesystem::path& config) {
    const pid_t pid = ::fork();
    if (pid == 0) {
        kcf::log::logger().use_stderr();
        kcf::linux_input::DaemonOptions options;
        options.config_path = config;
        options.only_device_name = device;
        options.log_level_override = kcf::LogLevel::Debug;
        ::setenv("XDG_RUNTIME_DIR", config.parent_path().c_str(), 1);
        ::_exit(kcf::linux_input::run_daemon(options));
    }
    return pid;
}

struct Fixture {
    std::string name = "kcf-integration-" + std::to_string(::getpid());
    std::filesystem::path dir = std::filesystem::temp_directory_path() / ("kcf-it-" + std::to_string(::getpid()));
    std::filesystem::path config = dir / "config.toml";
    FakeKeyboard keyboard{name};
    pid_t daemon = -1;
    int mirror = -1;

    Fixture() {
        std::filesystem::create_directories(dir);
        std::ofstream(config) << "chatter_threshold_ms = 30\n";
    }
    ~Fixture() {
        if (mirror >= 0) {
            ::close(mirror);
        }
        if (daemon > 0) {
            ::kill(daemon, SIGKILL);
            ::waitpid(daemon, nullptr, 0);
        }
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    bool start() {
        if (!keyboard.ok() || !find_node(name, 3s)) {
            return false;
        }
        daemon = start_daemon(name, config);
        const auto node = find_node(std::string(kcf::linux_input::kVirtualDevicePrefix) + name, 5s);
        if (!node) {
            return false;
        }
        mirror = ::open(node->c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        sleep_ms(300);  // let the daemon finish taking over the device
        collect(mirror, 50ms);
        return mirror >= 0;
    }
};

bool environment_ready() {
    return ::access("/dev/uinput", R_OK | W_OK) == 0;
}

}  // namespace

TEST(LinuxIntegration, FiltersChatterAndKeepsEverythingElse) {
    Fixture f;
    ASSERT_TRUE(f.start());

    // A plain tap passes through.
    f.keyboard.key(KEY_A, 1);
    sleep_ms(50);
    f.keyboard.key(KEY_A, 0);
    EXPECT_EQ(collect(f.mirror, 150ms), (std::vector<KeyRecord>{{KEY_A, 1}, {KEY_A, 0}}));

    // Release bounce: one keystroke comes out.
    f.keyboard.key(KEY_B, 1);
    sleep_ms(60);
    f.keyboard.key(KEY_B, 0);
    sleep_ms(3);
    f.keyboard.key(KEY_B, 1);
    sleep_ms(2);
    f.keyboard.key(KEY_B, 0);
    EXPECT_EQ(collect(f.mirror, 150ms), (std::vector<KeyRecord>{{KEY_B, 1}, {KEY_B, 0}}));

    // An intentional double tap 80 ms apart is two keystrokes.
    f.keyboard.key(KEY_C, 1);
    sleep_ms(50);
    f.keyboard.key(KEY_C, 0);
    sleep_ms(80);
    f.keyboard.key(KEY_C, 1);
    sleep_ms(50);
    f.keyboard.key(KEY_C, 0);
    EXPECT_EQ(collect(f.mirror, 150ms), (std::vector<KeyRecord>{{KEY_C, 1}, {KEY_C, 0}, {KEY_C, 1}, {KEY_C, 0}}));

    // Shift is released before the next letter reaches applications.
    f.keyboard.key(KEY_LEFTSHIFT, 1);
    sleep_ms(30);
    f.keyboard.key(KEY_E, 1);
    sleep_ms(30);
    f.keyboard.key(KEY_E, 0);
    sleep_ms(5);
    f.keyboard.key(KEY_LEFTSHIFT, 0);
    sleep_ms(5);
    f.keyboard.key(KEY_F, 1);
    sleep_ms(30);
    f.keyboard.key(KEY_F, 0);
    EXPECT_EQ(collect(f.mirror, 150ms), (std::vector<KeyRecord>{{KEY_LEFTSHIFT, 1},
                                                                 {KEY_E, 1},
                                                                 {KEY_E, 0},
                                                                 {KEY_LEFTSHIFT, 0},
                                                                 {KEY_F, 1},
                                                                 {KEY_F, 0}}));
}

TEST(LinuxIntegration, HeldKeyKeepsRepeating) {
    Fixture f;
    ASSERT_TRUE(f.start());
    f.keyboard.key(KEY_D, 1);
    sleep_ms(700);  // longer than the kernel's 250 ms repeat delay
    f.keyboard.key(KEY_D, 0);
    const auto events = collect(f.mirror, 150ms);
    const auto repeats = std::count_if(events.begin(), events.end(), [](const KeyRecord& r) { return r.value == 2; });
    EXPECT_GE(repeats, 5);
    EXPECT_EQ(without_repeats(events), (std::vector<KeyRecord>{{KEY_D, 1}, {KEY_D, 0}}));
}

TEST(LinuxIntegration, KeyboardIsReturnedWhenTheFilterDies) {
    Fixture f;
    ASSERT_TRUE(f.start());
    const auto source = find_node(f.name, 1s);
    ASSERT_TRUE(source.has_value());

    // While the filter runs, it holds the exclusive grab.
    const int fd = ::open(source->c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    ASSERT_TRUE(fd >= 0);
    EXPECT_TRUE(kcf::linux_input::device_ioctl(fd, EVIOCGRAB, 1) != 0);

    // Simulate a crash: no cleanup code runs at all.
    ::kill(f.daemon, SIGKILL);
    ::waitpid(f.daemon, nullptr, 0);
    f.daemon = -1;
    sleep_ms(100);

    // The kernel released the grab and removed the virtual keyboard.
    EXPECT_EQ(kcf::linux_input::device_ioctl(fd, EVIOCGRAB, 1), 0);
    kcf::linux_input::device_ioctl(fd, EVIOCGRAB, 0);
    ::close(fd);
    EXPECT_FALSE(find_node(std::string(kcf::linux_input::kVirtualDevicePrefix) + f.name, 500ms).has_value());
}

TEST(LinuxIntegration, GracefulStopDeliversHeldBackRelease) {
    Fixture f;
    ASSERT_TRUE(f.start());
    std::vector<KeyRecord> events;
    std::thread reader([&] { events = collect(f.mirror, 300ms); });
    f.keyboard.key(KEY_G, 1);
    sleep_ms(40);
    f.keyboard.key(KEY_G, 0);
    sleep_ms(2);
    ::kill(f.daemon, SIGTERM);  // inside the 30 ms window: the release is still held back
    int status = 0;
    ::waitpid(f.daemon, &status, 0);
    f.daemon = -1;
    reader.join();
    EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    EXPECT_EQ(events, (std::vector<KeyRecord>{{KEY_G, 1}, {KEY_G, 0}}));
}

int main(int argc, char** argv) {
    if (!environment_ready()) {
        std::printf("skipped: /dev/uinput is not accessible (run as root with the uinput module loaded)\n");
        return 77;
    }
    return kcf_test::run_all(argc, argv);
}
