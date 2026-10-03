// SPDX-License-Identifier: MIT
#include "macos/keyboard_interceptor.h"

#include <exception>
#include <unistd.h>

#include "common/logging.h"
#include "macos/event_conversion.h"

namespace kcf::macos {

static_assert(static_cast<std::uint32_t>(NativeType::KeyDown) == kCGEventKeyDown);
static_assert(static_cast<std::uint32_t>(NativeType::KeyUp) == kCGEventKeyUp);
static_assert(static_cast<std::uint32_t>(NativeType::FlagsChanged) == kCGEventFlagsChanged);
static_assert(flags::kShift == kCGEventFlagMaskShift);
static_assert(flags::kControl == kCGEventFlagMaskControl);
static_assert(flags::kAlternate == kCGEventFlagMaskAlternate);
static_assert(flags::kCommand == kCGEventFlagMaskCommand);
static_assert(flags::kAlphaShift == kCGEventFlagMaskAlphaShift);
static_assert(flags::kSecondaryFn == kCGEventFlagMaskSecondaryFn);

bool has_accessibility_permission() noexcept {
    return AXIsProcessTrusted();
}

void request_accessibility_permission() noexcept {
    const void* keys[] = {kAXTrustedCheckOptionPrompt};
    const void* values[] = {kCFBooleanTrue};
    CFRef<CFDictionaryRef> options(CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
                                                      &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks));
    if (options) {
        AXIsProcessTrustedWithOptions(options.get());
    }
}

MacKeyboardInterceptor::MacKeyboardInterceptor(MacKeyboardOutput& output, const MachClock& clock)
    : output_(output), clock_(clock), own_pid_(::getpid()) {}

MacKeyboardInterceptor::~MacKeyboardInterceptor() {
    stop();
}

InterceptorStartResult MacKeyboardInterceptor::start(IKeyEventHandler& handler) {
    if (tap_) {
        return {};
    }
    handler_ = &handler;
    failed_ = false;

    // Key down/up only. Modifier keys (flagsChanged) are deliberately not intercepted: chatter on a
    // modifier produces no extra characters, and leaving them alone means the filter can never
    // affect the system's modifier state.
    const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp);

    // HID level: before session-level remappers and every application. Fall back to the session
    // level, which is equivalent for our purposes, if the HID location is refused.
    CFMachPortRef tap = CGEventTapCreate(kCGHIDEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask,
                                         &MacKeyboardInterceptor::callback, this);
    if (tap == nullptr) {
        tap = CGEventTapCreate(kCGSessionEventTap, kCGHeadInsertEventTap, kCGEventTapOptionDefault, mask,
                               &MacKeyboardInterceptor::callback, this);
    }
    if (tap == nullptr) {
        handler_ = nullptr;
        if (!has_accessibility_permission()) {
            return {InterceptorError::PermissionDenied,
                    "Accessibility permission is required to filter keyboard events"};
        }
        return {InterceptorError::Unavailable, "CGEventTapCreate failed although Accessibility access is granted"};
    }
    tap_.reset(tap);

    source_.reset(CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap_.get(), 0));
    if (!source_) {
        stop();
        return {InterceptorError::Unavailable, "could not create a run loop source for the event tap"};
    }
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source_.get(), kCFRunLoopCommonModes);
    CGEventTapEnable(tap_.get(), true);
    return {};
}

void MacKeyboardInterceptor::stop() noexcept {
    if (tap_) {
        CGEventTapEnable(tap_.get(), false);
    }
    if (source_) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source_.get(), kCFRunLoopCommonModes);
        source_.reset();
    }
    if (tap_) {
        CFMachPortInvalidate(tap_.get());
        tap_.reset();
    }
    handler_ = nullptr;
}

CGEventRef MacKeyboardInterceptor::callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event,
                                            void* user_info) noexcept {
    auto* self = static_cast<MacKeyboardInterceptor*>(user_info);
    try {
        return self->handle(proxy, type, event);
    } catch (const std::exception& e) {
        log::error("unexpected error while handling a keyboard event: ", e.what());
    } catch (...) {
        log::error("unexpected error while handling a keyboard event");
    }
    self->output_.leave_callback();
    return event;  // fail open: never swallow input because of our own bug
}

CGEventRef MacKeyboardInterceptor::handle(CGEventTapProxy proxy, CGEventType type, CGEventRef event) {
    if (type == kCGEventTapDisabledByTimeout || type == kCGEventTapDisabledByUserInput) {
        // macOS disables taps that respond too slowly, and while secure input is active. Events
        // during that period bypassed us, so per-key state is stale.
        if (handler_ != nullptr) {
            handler_->on_events_lost();
        }
        if (tap_) {
            CGEventTapEnable(tap_.get(), true);
            ++reenabled_;
            if (!CGEventTapIsEnabled(tap_.get())) {
                failed_ = true;
            }
        }
        log::info(type == kCGEventTapDisabledByTimeout ? "event tap timed out; re-enabled"
                                                       : "event tap disabled by user input; re-enabled");
        return event;
    }

    if (handler_ == nullptr || event == nullptr ||
        (type != kCGEventKeyDown && type != kCGEventKeyUp && type != kCGEventFlagsChanged)) {
        return event;
    }

    // Our own re-injected events (recognised by marker and, independently, by the posting process),
    // and anything generated by software rather than a keyboard.
    if (CGEventGetIntegerValueField(event, kCGEventSourceUserData) == kInjectedEventMarker ||
        (pass_own_events_ && CGEventGetIntegerValueField(event, kCGEventSourceUnixProcessID) == own_pid_)) {
        return event;
    }
    if (CGEventGetIntegerValueField(event, kCGEventSourceStateID) != kCGEventSourceStateHIDSystemState) {
        return event;
    }

    const ResolvedTimestamp time = clock_.resolve(CGEventGetTimestamp(event));
    auto& seen = timestamp_sources_[static_cast<std::size_t>(time.source)];
    if (seen++ == 0) {
        log::info("keyboard event timing: ", to_string(time.source));
    }
    const auto keycode = static_cast<std::uint16_t>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
    const bool autorepeat = CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) != 0;
    const std::optional<KeyEvent> key_event =
        to_key_event(static_cast<NativeType>(type), keycode, CGEventGetFlags(event), autorepeat, time.time);
    if (!key_event) {
        return event;
    }

    output_.enter_callback(proxy);
    const Decision decision = handler_->on_key_event(*key_event);
    output_.leave_callback();
    if (dry_run_) {
        return event;
    }

    switch (decision) {
        case Decision::Accept:
            return event;
        case Decision::Defer:
            output_.stash(key_event->code, event);
            return nullptr;
        case Decision::Reject:
            return nullptr;
    }
    return event;
}

}  // namespace kcf::macos
