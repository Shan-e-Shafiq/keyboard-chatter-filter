// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>

#include "keyboard_filter/key.h"

namespace kcf {

// Monotonic time in nanoseconds. The epoch is defined by the platform adapter (boot time on all
// supported platforms); the core only ever compares timestamps produced by the same adapter.
using Timestamp = std::chrono::nanoseconds;
using Duration = std::chrono::nanoseconds;

// Adapter-assigned identifier of the input device an event came from. Not every platform can
// attribute events to a device (macOS event taps and Windows low-level hooks cannot), in which
// case the value is kUnknownDevice.
using DeviceId = std::uint32_t;
inline constexpr DeviceId kUnknownDevice = 0;

enum class KeyRole : std::uint8_t {
    Regular,
    Modifier,  // Shift, Control, Alt/Option, Meta/Command/Windows, Fn
};

// Platform-independent keyboard event. Deliberately small and trivially copyable: it is created
// for every hardware event and must never allocate.
struct KeyEvent {
    KeyCode code = 0;
    KeyAction action = KeyAction::Down;
    KeyRole role = KeyRole::Regular;
    ModifierState modifiers{};
    DeviceId device = kUnknownDevice;
    Timestamp timestamp{};

    [[nodiscard]] constexpr bool is_modifier() const noexcept { return role == KeyRole::Modifier; }

    friend constexpr bool operator==(const KeyEvent&, const KeyEvent&) noexcept = default;
};

}  // namespace kcf
