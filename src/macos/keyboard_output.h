// SPDX-License-Identifier: MIT
#pragma once

#include <ApplicationServices/ApplicationServices.h>

#include <array>
#include <cstdint>

#include "keyboard_filter/keyboard_io.h"
#include "macos/cf_ref.h"

namespace kcf::macos {

// Marker stored in kCGEventSourceUserData of events we post, so the tap recognises and passes its
// own re-injected events.
inline constexpr std::int64_t kInjectedEventMarker = 0x4B43465F4F555401;  // "KCF_OUT\x01"

// Delivers held-back key events to applications. When the filter defers a native event the
// interceptor stashes a copy here; emitting the KeyEvent later posts that exact copy, so the event
// applications receive is the original one (same key code, flags, keyboard type, timestamp).
class MacKeyboardOutput final : public IKeyboardOutput {
public:
    MacKeyboardOutput() = default;
    ~MacKeyboardOutput() override = default;
    MacKeyboardOutput(const MacKeyboardOutput&) = delete;
    MacKeyboardOutput& operator=(const MacKeyboardOutput&) = delete;

    void emit(const KeyEvent& event) override;

    // Keeps a copy of a native event the filter just deferred.
    void stash(KeyCode code, CGEventRef event);

    // While an event-tap callback runs, events are inserted ahead of the event being processed
    // (CGEventTapPostEvent); otherwise they are posted at the HID level (CGEventPost).
    void enter_callback(CGEventTapProxy proxy) noexcept { proxy_ = proxy; }
    void leave_callback() noexcept { proxy_ = nullptr; }

    // Drops stashed copies (after the filter state was reset and all releases were emitted).
    void clear() noexcept;

    // Dry run: the filter's decisions are only counted; nothing is ever posted.
    void set_dry_run(bool dry_run) noexcept { dry_run_ = dry_run; }

    [[nodiscard]] std::uint64_t post_failures() const noexcept { return post_failures_; }

private:
    static constexpr std::size_t kSlots = 256;  // macOS virtual key codes fit in 0..0x7F

    std::array<CFRef<CGEventRef>, kSlots> stash_{};
    CGEventTapProxy proxy_ = nullptr;
    std::uint64_t post_failures_ = 0;
    bool dry_run_ = false;
};

}  // namespace kcf::macos
