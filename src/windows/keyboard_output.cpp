// SPDX-License-Identifier: MIT
#include "windows/keyboard_output.h"

#include "common/logging.h"
#include "windows/win_util.h"

namespace kcf::windows {

WinKeyboardOutput::WinKeyboardOutput() : stash_(kKeyCodeLimit) {
    during_hook_.reserve(8);
    queue_.reserve(16);
}

void WinKeyboardOutput::stash(KeyCode code, const HookEvent& event) noexcept {
    if (code < kKeyCodeLimit) {
        stash_[code] = event;
        stashed_.set(code);
    }
}

INPUT WinKeyboardOutput::make_input(const HookEvent& event, bool key_up) const noexcept {
    const InjectionSpec spec = injection_for(event, key_up);
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = spec.vk;
    input.ki.wScan = spec.scan;
    input.ki.dwFlags = spec.flags;
    input.ki.time = 0;  // let the system stamp it
    input.ki.dwExtraInfo = kInjectedEventMarker;
    return input;
}

void WinKeyboardOutput::emit(const KeyEvent& event) {
    HookEvent native{};
    if (event.code < kKeyCodeLimit && stashed_.test(event.code)) {
        native = stash_[event.code];
        stashed_.reset(event.code);
    } else if (event.code >= 0x400 && event.code < 0x500) {
        native.vk_code = event.code & 0xFFu;
    } else {
        native.scan_code = event.code & 0xFFu;
        native.flags = (event.code & 0x100u) != 0 ? llkhf::kExtended : 0u;
        native.vk_code = ::MapVirtualKeyW(native.scan_code | ((event.code & 0x100u) != 0 ? 0xE000u : 0u),
                                          MAPVK_VSC_TO_VK_EX);
    }
    const INPUT input = make_input(native, event.action == KeyAction::Up);
    if (in_hook_) {
        during_hook_.push_back(input);
        modifier_flushed_ = modifier_flushed_ || event.is_modifier();
    } else {
        queue_.push_back(input);
    }
}

void WinKeyboardOutput::begin_hook() noexcept {
    in_hook_ = true;
    modifier_flushed_ = false;
    during_hook_.clear();
}

bool WinKeyboardOutput::end_hook() {
    in_hook_ = false;
    const bool reinject_current = modifier_flushed_;
    queue_.insert(queue_.end(), during_hook_.begin(), during_hook_.end());
    during_hook_.clear();
    return reinject_current;
}

void WinKeyboardOutput::queue_reinjection(const HookEvent& event, bool key_up) {
    queue_.push_back(make_input(event, key_up));
}

void WinKeyboardOutput::send_queued() {
    if (queue_.empty()) {
        return;
    }
    const UINT sent = ::SendInput(static_cast<UINT>(queue_.size()), queue_.data(), sizeof(INPUT));
    if (sent != queue_.size()) {
        // Typically UIPI (an elevated window has focus) or the secure desktop is active.
        if (send_failures_++ % 100 == 0) {
            log::warning("SendInput delivered ", sent, " of ", queue_.size(), " event(s): ", last_error_message());
        }
    }
    queue_.clear();
}

}  // namespace kcf::windows
