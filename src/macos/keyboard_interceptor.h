// SPDX-License-Identifier: MIT
#pragma once

#include <ApplicationServices/ApplicationServices.h>

#include <cstdint>

#include "keyboard_filter/keyboard_io.h"
#include "macos/cf_ref.h"
#include "macos/keyboard_output.h"
#include "macos/mach_clock.h"

namespace kcf::macos {

// System-wide keyboard interception with a Quartz event tap (CGEventTapCreate) placed at the HID
// level, ahead of every application and session-level tap. Only physical keyboard events are
// filtered: events posted by software (remote control, password managers, our own re-injections)
// pass untouched.
class MacKeyboardInterceptor final : public IKeyboardInterceptor {
public:
    MacKeyboardInterceptor(MacKeyboardOutput& output, const MachClock& clock);
    ~MacKeyboardInterceptor() override;
    MacKeyboardInterceptor(const MacKeyboardInterceptor&) = delete;
    MacKeyboardInterceptor& operator=(const MacKeyboardInterceptor&) = delete;

    InterceptorStartResult start(IKeyEventHandler& handler) override;
    void stop() noexcept override;
    [[nodiscard]] bool is_active() const noexcept override { return static_cast<bool>(tap_); }

    // Number of times macOS disabled the tap (callback too slow, or secure input) and we re-enabled it.
    [[nodiscard]] std::uint64_t reenable_count() const noexcept { return reenabled_; }
    // True if the tap was disabled and could not be re-enabled; the daemon tears it down and retries.
    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    static CGEventRef callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* user_info) noexcept;
    CGEventRef handle(CGEventTapProxy proxy, CGEventType type, CGEventRef event);

    MacKeyboardOutput& output_;
    const MachClock& clock_;
    IKeyEventHandler* handler_ = nullptr;
    CFRef<CFMachPortRef> tap_;
    CFRef<CFRunLoopSourceRef> source_;
    std::uint64_t reenabled_ = 0;
    bool failed_ = false;
};

// Whether this process may create an active (filtering) keyboard event tap. Never prompts.
[[nodiscard]] bool has_accessibility_permission() noexcept;

// Asks macOS to show its Accessibility permission prompt for this executable (adds it, switched
// off, to System Settings > Privacy & Security > Accessibility). Cannot grant anything itself.
void request_accessibility_permission() noexcept;

}  // namespace kcf::macos
