// SPDX-License-Identifier: MIT
//
// Pure translation rules for macOS keyboard events. Deliberately free of Apple headers so the rules
// are unit-tested on every CI platform; keyboard_interceptor.cpp feeds them values read from
// CGEventRef. Constants mirror <IOKit/hidsystem/IOLLEvent.h>, <CoreGraphics/CGEventTypes.h> and
// Carbon's <HIToolbox/Events.h> (kVK_*); they are part of the stable macOS ABI.
#pragma once

#include <cstdint>
#include <optional>

#include "keyboard_filter/key_event.h"

namespace kcf::macos {

namespace flags {
// Device-independent masks (CGEventFlags).
inline constexpr std::uint64_t kAlphaShift = 0x00010000;
inline constexpr std::uint64_t kShift = 0x00020000;
inline constexpr std::uint64_t kControl = 0x00040000;
inline constexpr std::uint64_t kAlternate = 0x00080000;
inline constexpr std::uint64_t kCommand = 0x00100000;
inline constexpr std::uint64_t kSecondaryFn = 0x00800000;
// Device-dependent masks (NX_DEVICE*KEYMASK) distinguishing left and right keys.
inline constexpr std::uint64_t kDeviceLeftControl = 0x00000001;
inline constexpr std::uint64_t kDeviceLeftShift = 0x00000002;
inline constexpr std::uint64_t kDeviceRightShift = 0x00000004;
inline constexpr std::uint64_t kDeviceLeftCommand = 0x00000008;
inline constexpr std::uint64_t kDeviceRightCommand = 0x00000010;
inline constexpr std::uint64_t kDeviceLeftAlternate = 0x00000020;
inline constexpr std::uint64_t kDeviceRightAlternate = 0x00000040;
inline constexpr std::uint64_t kDeviceRightControl = 0x00002000;
}  // namespace flags

namespace keycodes {
inline constexpr std::uint16_t kRightCommand = 0x36;
inline constexpr std::uint16_t kCommand = 0x37;
inline constexpr std::uint16_t kShift = 0x38;
inline constexpr std::uint16_t kCapsLock = 0x39;
inline constexpr std::uint16_t kOption = 0x3A;
inline constexpr std::uint16_t kControl = 0x3B;
inline constexpr std::uint16_t kRightShift = 0x3C;
inline constexpr std::uint16_t kRightOption = 0x3D;
inline constexpr std::uint16_t kRightControl = 0x3E;
inline constexpr std::uint16_t kFunction = 0x3F;
}  // namespace keycodes

// CGEventType values for the events the tap subscribes to.
enum class NativeType : std::uint32_t {
    KeyDown = 10,
    KeyUp = 11,
    FlagsChanged = 12,
};

[[nodiscard]] ModifierState modifiers_from_flags(std::uint64_t flags) noexcept;

// For a flagsChanged event: whether `keycode` (a modifier key) is now down. std::nullopt for keys
// that are not filtered as modifiers (Caps Lock toggles inside the HID system before any tap sees
// it, so dropping its event would desynchronise the lock state).
[[nodiscard]] std::optional<bool> modifier_is_down(std::uint16_t keycode, std::uint64_t flags) noexcept;

// Converts the fields of a native event into a KeyEvent, or std::nullopt if the event must pass
// through untouched.
[[nodiscard]] std::optional<KeyEvent> to_key_event(NativeType type, std::uint16_t keycode, std::uint64_t flags,
                                                   bool autorepeat, Timestamp timestamp) noexcept;

// Converts a CGEventTimestamp to nanoseconds of the mach_absolute_time clock. The value is
// documented as nanoseconds, but some systems report mach ticks; whichever interpretation lies
// closer to "now" is the right one (events are at most seconds old, while the two scales differ by
// a factor of ~41 on Apple silicon).
[[nodiscard]] Timestamp event_timestamp_to_ns(std::uint64_t event_timestamp, std::uint64_t now_ticks,
                                              std::uint32_t timebase_numer, std::uint32_t timebase_denom) noexcept;

[[nodiscard]] std::uint64_t ticks_to_ns(std::uint64_t ticks, std::uint32_t numer, std::uint32_t denom) noexcept;

}  // namespace kcf::macos
