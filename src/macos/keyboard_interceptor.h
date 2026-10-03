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

    // Dry run: events are classified (and counted) but always passed through unchanged. Must be set
    // before start().
    void set_dry_run(bool dry_run) noexcept { dry_run_ = dry_run; }

    // Events posted by this process are passed untouched (they are our own re-injections). Tests that
    // post synthetic "hardware" events from the same process turn this off.
    void set_pass_own_events(bool pass) noexcept { pass_own_events_ = pass; }

    // How many events used each timestamp source (diagnostics; see resolve_event_timestamp()).
    [[nodiscard]] std::uint64_t timestamp_source_count(TimestampSource source) const noexcept {
        return timestamp_sources_[static_cast<std::size_t>(source)];
    }

private:
    static CGEventRef callback(CGEventTapProxy proxy, CGEventType type, CGEventRef event, void* user_info) noexcept;
    CGEventRef handle(CGEventTapProxy proxy, CGEventType type, CGEventRef event);

    MacKeyboardOutput& output_;
    const MachClock& clock_;
    IKeyEventHandler* handler_ = nullptr;
    CFRef<CFMachPortRef> tap_;
    CFRef<CFRunLoopSourceRef> source_;
    std::uint64_t reenabled_ = 0;
    std::uint64_t timestamp_sources_[3] = {0, 0, 0};
    std::int64_t own_pid_ = 0;
    bool failed_ = false;
    bool dry_run_ = false;
    bool pass_own_events_ = true;
};

// Whether this process may create an active (filtering) keyboard event tap. Never prompts.
[[nodiscard]] bool has_accessibility_permission() noexcept;

// Asks macOS to show its Accessibility permission prompt for this executable (adds it, switched
// off, to System Settings > Privacy & Security > Accessibility). Cannot grant anything itself.
void request_accessibility_permission() noexcept;

}  // namespace kcf::macos
