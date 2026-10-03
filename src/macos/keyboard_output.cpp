// SPDX-License-Identifier: MIT
#include "macos/keyboard_output.h"

#include "common/logging.h"

namespace kcf::macos {

void MacKeyboardOutput::stash(KeyCode code, CGEventRef event) {
    if (code >= kSlots) {
        return;
    }
    stash_[code].reset(CGEventCreateCopy(event));
}

void MacKeyboardOutput::clear() noexcept {
    for (auto& slot : stash_) {
        slot.reset();
    }
}

void MacKeyboardOutput::emit(const KeyEvent& event) {
    CFRef<CGEventRef> native;
    if (event.code < kSlots && stash_[event.code]) {
        native = std::move(stash_[event.code]);
    } else {
        // Only reachable if state got out of sync; synthesise a plain key event so the key is never
        // left stuck from the applications' point of view.
        native.reset(CGEventCreateKeyboardEvent(nullptr, static_cast<CGKeyCode>(event.code),
                                                event.action != KeyAction::Up));
        if (!native) {
            ++post_failures_;
            log::error("could not create a replacement keyboard event");
            return;
        }
        log::debug("posting a synthesised key event (no stashed original)");
    }

    CGEventSetIntegerValueField(native.get(), kCGEventSourceUserData, kInjectedEventMarker);
    if (proxy_ != nullptr) {
        // Enters the event stream before the event currently in the tap callback, and is seen
        // only by taps after ours.
        CGEventTapPostEvent(proxy_, native.get());
    } else {
        CGEventPost(kCGHIDEventTap, native.get());
    }
}

}  // namespace kcf::macos
