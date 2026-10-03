// SPDX-License-Identifier: MIT
//
// Thin RAII wrappers over the kernel's evdev (/dev/input/event*) and uinput (/dev/uinput)
// interfaces, using the documented ioctls directly (no libevdev dependency, so the release binary
// is fully static). See Documentation/input/input.rst and uinput.rst in the kernel tree.
#pragma once

#include <linux/input.h>
#include <sys/ioctl.h>

#include <bitset>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "common/posix/posix.h"
#include "linux/device_classifier.h"

namespace kcf::linux_input {

// glibc declares ioctl(int, unsigned long, ...), musl ioctl(int, int, ...). Request numbers are
// passed through this helper so both compile cleanly.
template <typename... Args>
int device_ioctl(int fd, unsigned long request, Args... args) noexcept {
#if defined(__GLIBC__)
    return ::ioctl(fd, request, args...);
#else
    return ::ioctl(fd, static_cast<int>(request), args...);
#endif
}

// An opened evdev device node.
class EvdevDevice {
public:
    // Opens the node read/write (LEDs are written back to it), non-blocking.
    static std::optional<EvdevDevice> open(const std::filesystem::path& path, std::string& error);

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }
    [[nodiscard]] dev_t device_number() const noexcept { return rdev_; }
    [[nodiscard]] const DeviceCapabilities& capabilities() const noexcept { return caps_; }

    // Exclusive access: no other reader (libinput, X, the console) receives events while grabbed.
    // The kernel drops the grab automatically when the descriptor is closed or the process dies.
    bool grab(std::string& error) noexcept;
    void ungrab() noexcept;
    [[nodiscard]] bool grabbed() const noexcept { return grabbed_; }

    // Keys currently held, straight from the kernel (EVIOCGKEY).
    [[nodiscard]] std::optional<std::bitset<code::kKeyCount>> pressed_keys() const noexcept;

    // Writes events (LED state) to the device.
    void write_events(std::span<const input_event> events) noexcept;

private:
    EvdevDevice(posix::UniqueFd fd, std::filesystem::path path) : fd_(std::move(fd)), path_(std::move(path)) {}
    bool read_capabilities(std::string& error);

    posix::UniqueFd fd_;
    std::filesystem::path path_;
    dev_t rdev_ = 0;
    DeviceCapabilities caps_;
    bool grabbed_ = false;
};

// A virtual keyboard created through /dev/uinput that mirrors one physical keyboard. Destroying it
// (or the process dying) removes the device; the kernel releases any keys still held on it.
class UinputDevice {
public:
    static std::optional<UinputDevice> create(const DeviceCapabilities& source, std::string& error);

    UinputDevice(UinputDevice&& other) noexcept = default;
    UinputDevice& operator=(UinputDevice&& other) noexcept = default;
    ~UinputDevice();

    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }

    // Writes a batch of events. Returns false if the kernel rejected the write.
    bool write_events(std::span<const input_event> events) noexcept;

private:
    explicit UinputDevice(posix::UniqueFd fd, std::string name) : fd_(std::move(fd)), name_(std::move(name)) {}

    posix::UniqueFd fd_;
    std::string name_;
};

[[nodiscard]] std::string uinput_name_for(const std::string& source_name);

}  // namespace kcf::linux_input
