// SPDX-License-Identifier: MIT
#include "linux/keyboard_output.h"

#include "common/logging.h"

namespace kcf::linux_input {

input_event make_input_event(unsigned type, unsigned code, int value) noexcept {
    input_event ev{};  // the kernel stamps the time of events written to uinput
    ev.type = static_cast<decltype(ev.type)>(type);
    ev.code = static_cast<decltype(ev.code)>(code);
    ev.value = value;
    return ev;
}

UinputKeyboardOutput::UinputKeyboardOutput(UinputDevice device) : device_(std::move(device)) {
    frame_.reserve(64);
    scratch_.reserve(4);
}

void UinputKeyboardOutput::emit(const KeyEvent& event) {
    const int value = event.action == KeyAction::Up ? 0 : event.action == KeyAction::Down ? 1 : 2;
    const input_event ev = make_input_event(EV_KEY, event.code, value);
    if (frame_open_) {
        frame_.push_back(ev);
        return;
    }
    scratch_.clear();
    scratch_.push_back(ev);
    scratch_.push_back(make_input_event(EV_SYN, SYN_REPORT, 0));
    write_now(scratch_);
}

void UinputKeyboardOutput::forward(const input_event& event) {
    frame_.push_back(event);
}

void UinputKeyboardOutput::end_frame() {
    frame_open_ = false;
    if (frame_.empty()) {
        return;
    }
    frame_.push_back(make_input_event(EV_SYN, SYN_REPORT, 0));
    write_now(frame_);
    frame_.clear();
}

void UinputKeyboardOutput::discard_frame() {
    frame_.clear();
    frame_open_ = false;
}

void UinputKeyboardOutput::write_now(std::vector<input_event>& events) {
    if (!device_.write_events(events)) {
        if (write_failures_++ == 0) {
            log::error("writing to the virtual keyboard failed: ", posix::errno_message(errno));
        }
    }
}

}  // namespace kcf::linux_input
