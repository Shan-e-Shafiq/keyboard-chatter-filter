// SPDX-License-Identifier: MIT
#include "linux/input_device.h"

#include <linux/uinput.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace kcf::linux_input {

// The classifier's constants must match the kernel ABI.
static_assert(ev::kKey == EV_KEY && ev::kRel == EV_REL && ev::kAbs == EV_ABS && ev::kMsc == EV_MSC);
static_assert(ev::kLed == EV_LED && ev::kRep == EV_REP && ev::kCount == EV_CNT);
static_assert(code::kKeyEnter == KEY_ENTER && code::kKeySpace == KEY_SPACE && code::kKeyKp7 == KEY_KP7);
static_assert(code::kKeyKp0 == KEY_KP0 && code::kKeyKpEnter == KEY_KPENTER);
static_assert(code::kBtnLeft == BTN_LEFT && code::kBtnTouch == BTN_TOUCH && code::kBtnToolFinger == BTN_TOOL_FINGER);
static_assert(code::kBtnToolPen == BTN_TOOL_PEN && code::kBtnStylus == BTN_STYLUS);
static_assert(code::kBtnJoystick == BTN_JOYSTICK && code::kBtnGamepad == BTN_GAMEPAD);
static_assert(code::kKeyCount == KEY_CNT && code::kRelCount == REL_CNT && code::kAbsCount == ABS_CNT);
static_assert(code::kRelX == REL_X && code::kRelY == REL_Y && code::kAbsX == ABS_X && code::kAbsY == ABS_Y);
static_assert(code::kAbsMtPositionX == ABS_MT_POSITION_X);
static_assert(code::kMscScan == MSC_SCAN && code::kMscCount == MSC_CNT && code::kLedCount == LED_CNT);
static_assert(bus::kUsb == BUS_USB && bus::kBluetooth == BUS_BLUETOOTH && bus::kVirtual == BUS_VIRTUAL);
static_assert(bus::kI8042 == BUS_I8042);
static_assert(UINPUT_MAX_NAME_SIZE == 80);

namespace {

template <std::size_t N>
bool read_bits(int fd, unsigned type, std::bitset<N>& out) {
    unsigned char buffer[(N + 7) / 8] = {};
    if (device_ioctl(fd, EVIOCGBIT(type, sizeof buffer), buffer) < 0) {
        return false;
    }
    for (std::size_t i = 0; i < N; ++i) {
        out[i] = (buffer[i / 8] >> (i % 8)) & 1;
    }
    return true;
}

}  // namespace

std::optional<EvdevDevice> EvdevDevice::open(const std::filesystem::path& path, std::string& error) {
    posix::UniqueFd fd(::open(path.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY));
    if (!fd && (errno == EACCES || errno == EPERM)) {
        // LED write-back is optional; reading is what matters.
        fd.reset(::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOCTTY));
    }
    if (!fd) {
        error = posix::errno_message(errno);
        return std::nullopt;
    }
    struct stat st {};
    if (::fstat(fd.get(), &st) != 0 || !S_ISCHR(st.st_mode)) {
        error = "not a character device";
        return std::nullopt;
    }
    EvdevDevice device(std::move(fd), path);
    device.rdev_ = st.st_rdev;
    if (!device.read_capabilities(error)) {
        return std::nullopt;
    }
    return device;
}

bool EvdevDevice::read_capabilities(std::string& error) {
    int version = 0;
    if (device_ioctl(fd_.get(), EVIOCGVERSION, &version) < 0) {
        error = "not an evdev device";
        return false;
    }
    char name[256] = {};
    if (device_ioctl(fd_.get(), EVIOCGNAME(sizeof name - 1), name) >= 0) {
        caps_.name = name;
    }
    input_id id{};
    if (device_ioctl(fd_.get(), EVIOCGID, &id) >= 0) {
        caps_.bustype = id.bustype;
        caps_.vendor = id.vendor;
        caps_.product = id.product;
    }
    if (!read_bits(fd_.get(), 0, caps_.events)) {
        error = "cannot read event types";
        return false;
    }
    if (caps_.events.test(EV_KEY)) {
        read_bits(fd_.get(), EV_KEY, caps_.keys);
    }
    if (caps_.events.test(EV_REL)) {
        read_bits(fd_.get(), EV_REL, caps_.rel);
    }
    if (caps_.events.test(EV_ABS)) {
        read_bits(fd_.get(), EV_ABS, caps_.abs);
    }
    if (caps_.events.test(EV_MSC)) {
        read_bits(fd_.get(), EV_MSC, caps_.msc);
    }
    if (caps_.events.test(EV_LED)) {
        read_bits(fd_.get(), EV_LED, caps_.leds);
    }
    // Event timestamps from the monotonic clock: immune to wall-clock changes (NTP, manual), and the
    // same clock the daemon's timers use.
    int clock = CLOCK_MONOTONIC;
    device_ioctl(fd_.get(), EVIOCSCLOCKID, &clock);
    return true;
}

bool EvdevDevice::grab(std::string& error) noexcept {
    if (grabbed_) {
        return true;
    }
    if (device_ioctl(fd_.get(), EVIOCGRAB, 1) != 0) {
        error = errno == EBUSY ? "already grabbed by another program" : posix::errno_message(errno);
        return false;
    }
    grabbed_ = true;
    return true;
}

void EvdevDevice::ungrab() noexcept {
    if (grabbed_) {
        device_ioctl(fd_.get(), EVIOCGRAB, 0);
        grabbed_ = false;
    }
}

std::optional<std::bitset<code::kKeyCount>> EvdevDevice::pressed_keys() const noexcept {
    unsigned char buffer[(code::kKeyCount + 7) / 8] = {};
    if (device_ioctl(fd_.get(), EVIOCGKEY(sizeof buffer), buffer) < 0) {
        return std::nullopt;
    }
    std::bitset<code::kKeyCount> keys;
    for (std::size_t i = 0; i < code::kKeyCount; ++i) {
        keys[i] = (buffer[i / 8] >> (i % 8)) & 1;
    }
    return keys;
}

void EvdevDevice::write_events(std::span<const input_event> events) noexcept {
    if (!events.empty()) {
        [[maybe_unused]] const ssize_t n = ::write(fd_.get(), events.data(), events.size_bytes());
    }
}

std::string uinput_name_for(const std::string& source_name) {
    std::string name(kVirtualDevicePrefix);
    name += source_name.empty() ? std::string("keyboard") : source_name;
    if (name.size() > UINPUT_MAX_NAME_SIZE - 1) {
        name.resize(UINPUT_MAX_NAME_SIZE - 1);
    }
    return name;
}

std::optional<UinputDevice> UinputDevice::create(const DeviceCapabilities& source, std::string& error) {
    posix::UniqueFd fd(::open("/dev/uinput", O_RDWR | O_NONBLOCK | O_CLOEXEC));
    if (!fd) {
        error = "/dev/uinput: " + posix::errno_message(errno);
        return std::nullopt;
    }
    auto set = [&](unsigned long request, unsigned value) { return device_ioctl(fd.get(), request, value) == 0; };

    bool ok = set(UI_SET_EVBIT, EV_SYN) && set(UI_SET_EVBIT, EV_KEY);
    for (unsigned k = 0; ok && k < code::kKeyCount; ++k) {
        if (source.keys.test(k)) {
            ok = set(UI_SET_KEYBIT, k);
        }
    }
    if (ok && source.events.test(EV_MSC) && source.msc.test(MSC_SCAN)) {
        ok = set(UI_SET_EVBIT, EV_MSC) && set(UI_SET_MSCBIT, MSC_SCAN);
    }
    if (ok && source.events.test(EV_LED)) {
        ok = set(UI_SET_EVBIT, EV_LED);
        for (unsigned l = 0; ok && l < LED_CNT; ++l) {
            if (source.leds.test(l)) {
                ok = set(UI_SET_LEDBIT, l);
            }
        }
    }
    if (ok && source.events.test(EV_REL)) {
        // Only non-pointer axes (keyboard wheels/dials) can be present on a device we grab.
        ok = set(UI_SET_EVBIT, EV_REL);
        for (unsigned r = 0; ok && r < code::kRelCount; ++r) {
            if (source.rel.test(r)) {
                ok = set(UI_SET_RELBIT, r);
            }
        }
    }
    // EV_REP is deliberately not enabled: the kernel would then generate its own auto-repeat on top
    // of the physical keyboard's repeat events we forward, doubling repeats on the text console.
    if (!ok) {
        error = "cannot configure the virtual keyboard: " + posix::errno_message(errno);
        return std::nullopt;
    }

    uinput_setup setup{};
    setup.id.bustype = BUS_VIRTUAL;  // never matches hwdb keymaps meant for the physical device
    setup.id.vendor = source.vendor;
    setup.id.product = source.product;
    setup.id.version = 1;
    const std::string name = uinput_name_for(source.name);
    std::memcpy(setup.name, name.c_str(), name.size() + 1);
    if (device_ioctl(fd.get(), UI_DEV_SETUP, &setup) != 0) {
        error = "UI_DEV_SETUP failed (Linux 4.5 or newer is required): " + posix::errno_message(errno);
        return std::nullopt;
    }
    if (device_ioctl(fd.get(), UI_DEV_CREATE) != 0) {
        error = "UI_DEV_CREATE failed: " + posix::errno_message(errno);
        return std::nullopt;
    }
    return UinputDevice(std::move(fd), name);
}

UinputDevice::~UinputDevice() {
    if (fd_) {
        device_ioctl(fd_.get(), UI_DEV_DESTROY);
    }
}

bool UinputDevice::write_events(std::span<const input_event> events) noexcept {
    if (events.empty()) {
        return true;
    }
    const auto* data = reinterpret_cast<const char*>(events.data());
    std::size_t remaining = events.size_bytes();
    while (remaining > 0) {
        const ssize_t n = ::write(fd_.get(), data, remaining);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        data += n;
        remaining -= static_cast<std::size_t>(n);
    }
    return true;
}

}  // namespace kcf::linux_input
