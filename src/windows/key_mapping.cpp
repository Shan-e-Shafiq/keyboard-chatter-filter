// SPDX-License-Identifier: MIT
#include "windows/key_mapping.h"

namespace kcf::windows {
namespace {

bool is_shift_vk(std::uint32_t v) noexcept {
    return v == vk::kShift || v == vk::kLShift || v == vk::kRShift;
}

}  // namespace

std::optional<KeyCode> physical_key_code(const HookEvent& e) noexcept {
    if (e.vk_code == vk::kPacket || e.vk_code == 0 || e.vk_code > 0xFF) {
        return std::nullopt;  // VK_PACKET carries Unicode text from SendInput, never a physical key
    }
    if (e.scan_code > 0xFF) {
        // Driver-generated companions such as the left Control that layouts with AltGr synthesise
        // (reported with scan code 0x21D). They follow the real key's events by themselves.
        return std::nullopt;
    }
    const bool extended = (e.flags & llkhf::kExtended) != 0;
    if (extended && is_shift_vk(e.vk_code)) {
        // "Fake shifts" (E0 2A / E0 AA) that keyboards wrap around navigation keys when NumLock is
        // on. They are part of another key's sequence, not a Shift press.
        return std::nullopt;
    }
    if (e.scan_code == 0) {
        return static_cast<KeyCode>(0x400 | e.vk_code);
    }
    return static_cast<KeyCode>(e.scan_code | (extended ? 0x100u : 0u));
}

bool is_modifier_vk(std::uint32_t v) noexcept {
    switch (v) {
        case vk::kShift:
        case vk::kControl:
        case vk::kMenu:
        case vk::kLShift:
        case vk::kRShift:
        case vk::kLControl:
        case vk::kRControl:
        case vk::kLMenu:
        case vk::kRMenu:
        case vk::kLWin:
        case vk::kRWin:
            return true;
        default:
            return false;
    }
}

void update_modifiers(ModifierState& state, std::uint32_t v, bool down) noexcept {
    switch (v) {
        case vk::kLShift: state.set(Modifier::LeftShift, down); break;
        case vk::kRShift: state.set(Modifier::RightShift, down); break;
        case vk::kLControl: state.set(Modifier::LeftControl, down); break;
        case vk::kRControl: state.set(Modifier::RightControl, down); break;
        case vk::kLMenu: state.set(Modifier::LeftAlt, down); break;
        case vk::kRMenu: state.set(Modifier::RightAlt, down); break;
        case vk::kLWin: state.set(Modifier::LeftMeta, down); break;
        case vk::kRWin: state.set(Modifier::RightMeta, down); break;
        default: break;
    }
}

std::optional<KeyEvent> to_key_event(const HookEvent& e, Timestamp timestamp, ModifierState modifiers) noexcept {
    if ((e.flags & (llkhf::kInjected | llkhf::kLowerIlInjected)) != 0) {
        return std::nullopt;
    }
    const std::optional<KeyCode> code = physical_key_code(e);
    if (!code) {
        return std::nullopt;
    }
    KeyEvent event;
    event.code = *code;
    event.action = (e.flags & llkhf::kUp) != 0 ? KeyAction::Up : KeyAction::Down;
    event.role = is_modifier_vk(e.vk_code) ? KeyRole::Modifier : KeyRole::Regular;
    event.modifiers = modifiers;
    event.timestamp = timestamp;
    return event;
}

InjectionSpec injection_for(const HookEvent& e, bool key_up) noexcept {
    InjectionSpec spec;
    spec.vk = static_cast<std::uint16_t>(e.vk_code);
    spec.scan = static_cast<std::uint16_t>(e.scan_code & 0xFF);
    if ((e.flags & llkhf::kExtended) != 0) {
        spec.flags |= keyeventf::kExtendedKey;
    }
    if (key_up) {
        spec.flags |= keyeventf::kKeyUp;
    }
    return spec;
}

}  // namespace kcf::windows
