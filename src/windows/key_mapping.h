// SPDX-License-Identifier: MIT
//
// Pure translation rules for Windows low-level keyboard hook events (KBDLLHOOKSTRUCT). Free of
// <windows.h> so they are unit-tested on every CI platform. Constants mirror <winuser.h>.
#pragma once

#include <cstdint>
#include <optional>

#include "keyboard_filter/key_event.h"

namespace kcf::windows {

namespace llkhf {
inline constexpr std::uint32_t kExtended = 0x01;
inline constexpr std::uint32_t kLowerIlInjected = 0x02;
inline constexpr std::uint32_t kInjected = 0x10;
inline constexpr std::uint32_t kAltDown = 0x20;
inline constexpr std::uint32_t kUp = 0x80;
}  // namespace llkhf

namespace vk {
inline constexpr std::uint32_t kShift = 0x10;
inline constexpr std::uint32_t kControl = 0x11;
inline constexpr std::uint32_t kMenu = 0x12;
inline constexpr std::uint32_t kCapital = 0x14;
inline constexpr std::uint32_t kLWin = 0x5B;
inline constexpr std::uint32_t kRWin = 0x5C;
inline constexpr std::uint32_t kLShift = 0xA0;
inline constexpr std::uint32_t kRShift = 0xA1;
inline constexpr std::uint32_t kLControl = 0xA2;
inline constexpr std::uint32_t kRControl = 0xA3;
inline constexpr std::uint32_t kLMenu = 0xA4;
inline constexpr std::uint32_t kRMenu = 0xA5;
inline constexpr std::uint32_t kPacket = 0xE7;
}  // namespace vk

namespace keyeventf {
inline constexpr std::uint32_t kExtendedKey = 0x0001;
inline constexpr std::uint32_t kKeyUp = 0x0002;
}  // namespace keyeventf

// The fields of KBDLLHOOKSTRUCT the filter uses.
struct HookEvent {
    std::uint32_t vk_code = 0;
    std::uint32_t scan_code = 0;
    std::uint32_t flags = 0;
};

// Identity of the physical key: the hardware scan code with the E0 prefix folded into bit 8.
// Events without a usable scan code fall back to 0x400 | virtual-key. std::nullopt for events that
// are artefacts of the keyboard driver rather than a physical key (see key_mapping.cpp).
[[nodiscard]] std::optional<KeyCode> physical_key_code(const HookEvent& event) noexcept;

[[nodiscard]] bool is_modifier_vk(std::uint32_t vk) noexcept;

// Tracks modifier keys from the events the hook observes (cheaper than GetAsyncKeyState on every
// event, and consistent with what the filter has seen).
void update_modifiers(ModifierState& state, std::uint32_t vk, bool down) noexcept;

// KeyEvent for a hardware event, or std::nullopt if it must pass through untouched (injected by
// software, or not attributable to a physical key). Windows does not flag auto-repeat in
// low-level hooks: repeats arrive as further key-downs and the filter recognises them.
[[nodiscard]] std::optional<KeyEvent> to_key_event(const HookEvent& event, Timestamp timestamp,
                                                   ModifierState modifiers) noexcept;

// Values for a KEYBDINPUT that reproduces `event` (used to re-inject held-back key-ups).
struct InjectionSpec {
    std::uint16_t vk = 0;
    std::uint16_t scan = 0;
    std::uint32_t flags = 0;
};
[[nodiscard]] InjectionSpec injection_for(const HookEvent& event, bool key_up) noexcept;

}  // namespace kcf::windows
