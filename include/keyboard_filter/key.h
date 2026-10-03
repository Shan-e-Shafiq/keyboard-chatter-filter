// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>

namespace kcf {

// Platform-defined identifier of a *physical* key. The core never interprets the value; it only
// uses it to keep independent state per key. Adapters choose a code space that is stable for a
// physical key:
//   macOS   - CGKeyCode (virtual key code, 0..0x7F in practice)
//   Windows - hardware scan code (with the extended-key prefix folded in), see windows/key_mapping.h
//   Linux   - evdev KEY_* code from <linux/input-event-codes.h>
using KeyCode = std::uint16_t;

// Codes at or above this limit are passed through without filtering. Keeping the per-key table a
// fixed size means the hot path never allocates.
inline constexpr std::size_t kKeyCodeLimit = 2048;

enum class KeyAction : std::uint8_t {
    Down,    // physical press (a make transition)
    Up,      // physical release (a break transition)
    Repeat,  // OS-generated auto-repeat, when the platform marks repeats explicitly
};

// Snapshot of modifier keys as reported by the OS alongside an event. Informational for the core
// (the filter treats modifier keys like every other key), but carried so that adapters can
// rebuild faithful native events and tests can verify modifiers survive filtering untouched.
enum class Modifier : std::uint16_t {
    None = 0,
    LeftShift = 1u << 0,
    RightShift = 1u << 1,
    LeftControl = 1u << 2,
    RightControl = 1u << 3,
    LeftAlt = 1u << 4,
    RightAlt = 1u << 5,
    LeftMeta = 1u << 6,  // Command (macOS), Windows key, Super
    RightMeta = 1u << 7,
    CapsLock = 1u << 8,
    Function = 1u << 9,
};

class ModifierState {
public:
    constexpr ModifierState() noexcept = default;
    constexpr explicit ModifierState(std::uint16_t bits) noexcept : bits_(bits) {}

    [[nodiscard]] constexpr bool has(Modifier m) const noexcept {
        return (bits_ & static_cast<std::uint16_t>(m)) != 0;
    }
    constexpr ModifierState& set(Modifier m, bool on = true) noexcept {
        if (on) {
            bits_ |= static_cast<std::uint16_t>(m);
        } else {
            bits_ &= static_cast<std::uint16_t>(~static_cast<std::uint16_t>(m));
        }
        return *this;
    }
    [[nodiscard]] constexpr std::uint16_t bits() const noexcept { return bits_; }
    [[nodiscard]] constexpr bool empty() const noexcept { return bits_ == 0; }

    friend constexpr bool operator==(ModifierState, ModifierState) noexcept = default;

private:
    std::uint16_t bits_ = 0;
};

[[nodiscard]] const char* to_string(KeyAction action) noexcept;

}  // namespace kcf
