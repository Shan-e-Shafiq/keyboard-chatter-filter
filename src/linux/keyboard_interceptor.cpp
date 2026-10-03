// SPDX-License-Identifier: MIT
#include "linux/keyboard_interceptor.h"

#include <cerrno>
#include <ctime>
#include <unistd.h>

#include "common/logging.h"

namespace kcf::linux_input {

Timestamp event_time(const input_event& ev) noexcept {
#ifdef input_event_sec
    const auto sec = static_cast<std::int64_t>(ev.input_event_sec);
    const auto usec = static_cast<std::int64_t>(ev.input_event_usec);
#else
    const auto sec = static_cast<std::int64_t>(ev.time.tv_sec);
    const auto usec = static_cast<std::int64_t>(ev.time.tv_usec);
#endif
    return Timestamp(sec * 1'000'000'000 + usec * 1'000);
}

bool is_modifier_key(unsigned code) noexcept {
    switch (code) {
        case KEY_LEFTSHIFT:
        case KEY_RIGHTSHIFT:
        case KEY_LEFTCTRL:
        case KEY_RIGHTCTRL:
        case KEY_LEFTALT:
        case KEY_RIGHTALT:
        case KEY_LEFTMETA:
        case KEY_RIGHTMETA:
        case KEY_FN:
            return true;
        default:
            return false;
    }
}

namespace {

void track_modifier(ModifierState& m, unsigned code, bool down) {
    switch (code) {
        case KEY_LEFTSHIFT: m.set(Modifier::LeftShift, down); break;
        case KEY_RIGHTSHIFT: m.set(Modifier::RightShift, down); break;
        case KEY_LEFTCTRL: m.set(Modifier::LeftControl, down); break;
        case KEY_RIGHTCTRL: m.set(Modifier::RightControl, down); break;
        case KEY_LEFTALT: m.set(Modifier::LeftAlt, down); break;
        case KEY_RIGHTALT: m.set(Modifier::RightAlt, down); break;
        case KEY_LEFTMETA: m.set(Modifier::LeftMeta, down); break;
        case KEY_RIGHTMETA: m.set(Modifier::RightMeta, down); break;
        case KEY_FN: m.set(Modifier::Function, down); break;
        default: break;
    }
}

}  // namespace

EvdevKeyboard::EvdevKeyboard(EvdevDevice device, UinputKeyboardOutput& output, DeviceId id,
                             std::function<void()> before_key_event)
    : device_(std::move(device)), output_(output), id_(id), before_key_event_(std::move(before_key_event)) {}

EvdevKeyboard::~EvdevKeyboard() {
    stop();
}

InterceptorStartResult EvdevKeyboard::start(IKeyEventHandler& handler) {
    handler_ = &handler;
    std::string error;
    if (!try_grab(error)) {
        handler_ = nullptr;
        return {InterceptorError::Unavailable, error};
    }
    return {};
}

void EvdevKeyboard::stop() noexcept {
    device_.ungrab();
    waiting_for_release_ = false;
    handler_ = nullptr;
}

bool EvdevKeyboard::try_grab(std::string& error) {
    const auto pressed = device_.pressed_keys();
    if (pressed && pressed->any()) {
        // Grabbing now would leave the held keys pressed forever in applications that saw the
        // press but would never see the release. Wait until everything is released.
        waiting_for_release_ = true;
        return true;
    }
    waiting_for_release_ = false;
    return device_.grab(error);
}

EvdevKeyboard::ReadStatus EvdevKeyboard::on_readable() {
    input_event buffer[64];
    while (true) {
        const ssize_t n = ::read(device_.fd(), buffer, sizeof buffer);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN) {
                return ReadStatus::Ok;
            }
            if (errno != ENODEV) {
                log::warning("reading ", device_.path().string(), " failed: ", posix::errno_message(errno));
            }
            return ReadStatus::Gone;
        }
        if (n == 0) {
            return ReadStatus::Gone;
        }
        const std::size_t count = static_cast<std::size_t>(n) / sizeof(input_event);
        for (std::size_t i = 0; i < count; ++i) {
            handle(buffer[i]);
        }
    }
}

void EvdevKeyboard::handle(const input_event& ev) {
    if (!device_.grabbed()) {
        // Not ours yet: the system receives these events directly from the device.
        if (waiting_for_release_ && handler_ != nullptr && ev.type == EV_SYN && ev.code == SYN_REPORT) {
            std::string error;
            if (!try_grab(error)) {
                log::warning("cannot take over ", device_.path().string(), ": ", error);
                waiting_for_release_ = false;
            } else if (device_.grabbed()) {
                log::info("now filtering \"", device_.capabilities().name, "\"");
            }
        }
        return;
    }

    if (dropping_) {
        // The kernel's buffer overflowed: everything up to the next report is unreliable.
        if (ev.type == EV_SYN && ev.code == SYN_REPORT) {
            dropping_ = false;
            resync();
        }
        return;
    }

    switch (ev.type) {
        case EV_SYN:
            if (ev.code == SYN_REPORT) {
                output_.end_frame();
                pending_scan_.reset();
            } else if (ev.code == SYN_DROPPED) {
                dropping_ = true;
                output_.discard_frame();
                pending_scan_.reset();
                log::debug("input buffer overflow on ", device_.path().string(), "; resynchronising");
            }
            return;
        case EV_MSC:
            output_.begin_frame();
            if (ev.code == MSC_SCAN) {
                pending_scan_ = ev;  // belongs to the key event that follows
            } else {
                output_.forward(ev);
            }
            return;
        case EV_KEY:
            output_.begin_frame();
            handle_key(ev);
            return;
        case EV_LED:
        case EV_REP:
        case EV_SND:
        case EV_FF:
            return;  // state echoes and output channels, not input
        default:
            output_.begin_frame();
            output_.forward(ev);
            return;
    }
}

void EvdevKeyboard::handle_key(const input_event& ev) {
    if (ev.value < 0 || ev.value > 2 || handler_ == nullptr) {
        pending_scan_.reset();
        return;
    }
    KeyEvent key;
    key.code = ev.code;
    key.action = ev.value == 0 ? KeyAction::Up : ev.value == 1 ? KeyAction::Down : KeyAction::Repeat;
    key.role = is_modifier_key(ev.code) ? KeyRole::Modifier : KeyRole::Regular;
    key.device = id_;
    key.timestamp = event_time(ev);
    if (ev.value != 2) {
        track_modifier(modifiers_, ev.code, ev.value == 1);
    }
    key.modifiers = modifiers_;

    if (before_key_event_) {
        before_key_event_();  // keeps the order of events across several keyboards
    }
    if (handler_->on_key_event(key) == Decision::Accept) {
        if (pending_scan_) {
            output_.forward(*pending_scan_);
        }
        output_.forward(ev);
    }
    pending_scan_.reset();
}

void EvdevKeyboard::resync() {
    if (handler_ == nullptr) {
        return;
    }
    const auto pressed = device_.pressed_keys();
    if (!pressed) {
        handler_->on_events_lost();
        return;
    }
    const DeviceCapabilities& caps = device_.capabilities();
    output_.begin_frame();
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    const Timestamp at(static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec);
    for (unsigned k = 0; k < code::kKeyCount; ++k) {
        if (caps.keys.test(k)) {
            handler_->on_key_state(static_cast<KeyCode>(k), pressed->test(k), at);
        }
    }
    output_.end_frame();
}

void EvdevKeyboard::on_virtual_led_feedback() {
    input_event buffer[16];
    UinputDevice& virtual_device = output_.device();
    while (true) {
        const ssize_t n = ::read(virtual_device.fd(), buffer, sizeof buffer);
        if (n <= 0) {
            return;
        }
        const std::size_t count = static_cast<std::size_t>(n) / sizeof(input_event);
        input_event leds[17];
        std::size_t used = 0;
        for (std::size_t i = 0; i < count; ++i) {
            if (buffer[i].type == EV_LED) {
                leds[used++] = make_input_event(EV_LED, buffer[i].code, buffer[i].value);
            }
        }
        if (used > 0) {
            leds[used++] = make_input_event(EV_SYN, SYN_REPORT, 0);
            device_.write_events(std::span<const input_event>(leds, used));
        }
    }
}

}  // namespace kcf::linux_input
