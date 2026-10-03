// SPDX-License-Identifier: MIT
#pragma once

#include <optional>
#include <string>

#include "keyboard_filter/chatter_filter.h"
#include "keyboard_filter/key_event.h"

namespace kcf {

// Delivers key events to applications. Platform implementations inject a native event (macOS
// CGEventTapPostEvent/CGEventPost, Windows SendInput, Linux uinput). Events the interceptor accepts
// are *not* routed through here; they simply continue through the OS untouched.
class IKeyboardOutput : public IEventSink {
public:
    virtual ~IKeyboardOutput() = default;
};

// Receives hardware key events from an interceptor and decides their fate.
class IKeyEventHandler {
public:
    virtual Decision on_key_event(const KeyEvent& event) = 0;

    // The interceptor may have missed events (event tap was disabled by the system, hook was
    // reinstalled, kernel buffer overflowed). Per-key state can no longer be trusted.
    virtual void on_events_lost() = 0;

    // The interceptor learned the authoritative physical state of a key after missing events (only
    // platforms that can query it, e.g. Linux after SYN_DROPPED). Defaults to a full reset.
    virtual void on_key_state(KeyCode code, bool physically_down, Timestamp now) {
        (void)code;
        (void)physically_down;
        (void)now;
        on_events_lost();
    }

protected:
    ~IKeyEventHandler() = default;
};

enum class InterceptorError {
    None,
    PermissionDenied,  // the OS refused access (macOS Accessibility, Linux device permissions)
    Unavailable,       // the mechanism could not be set up for another reason
};

struct InterceptorStartResult {
    InterceptorError error = InterceptorError::None;
    std::string message;

    [[nodiscard]] bool ok() const noexcept { return error == InterceptorError::None; }
};

// OS-specific source of hardware key events. Implementations must remove their interception
// mechanism in stop() and in their destructor (RAII), so a stopped or destroyed interceptor can
// never leave the keyboard captured.
class IKeyboardInterceptor {
public:
    virtual ~IKeyboardInterceptor() = default;

    virtual InterceptorStartResult start(IKeyEventHandler& handler) = 0;
    virtual void stop() noexcept = 0;
    [[nodiscard]] virtual bool is_active() const noexcept = 0;
};

// One-shot timer owned by the platform event loop. Scheduling replaces any earlier request;
// std::nullopt cancels it.
class IWakeupScheduler {
public:
    virtual void schedule_wakeup(std::optional<Timestamp> at) = 0;

protected:
    ~IWakeupScheduler() = default;
};

// Monotonic clock in the same time domain as the timestamps of intercepted events.
class IClock {
public:
    [[nodiscard]] virtual Timestamp now() const noexcept = 0;

protected:
    ~IClock() = default;
};

}  // namespace kcf
