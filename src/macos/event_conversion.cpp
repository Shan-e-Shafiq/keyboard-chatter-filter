// SPDX-License-Identifier: MIT
#include "macos/event_conversion.h"

namespace kcf::macos {
namespace {

struct ModifierKey {
    std::uint16_t keycode;
    std::uint64_t device_mask;  // this key's left/right-specific bit (0 for Fn)
    std::uint64_t pair_mask;    // device bits of both keys in the same class
    std::uint64_t class_mask;   // device-independent bit for the class
};

constexpr std::uint64_t kShiftPair = flags::kDeviceLeftShift | flags::kDeviceRightShift;
constexpr std::uint64_t kControlPair = flags::kDeviceLeftControl | flags::kDeviceRightControl;
constexpr std::uint64_t kAlternatePair = flags::kDeviceLeftAlternate | flags::kDeviceRightAlternate;
constexpr std::uint64_t kCommandPair = flags::kDeviceLeftCommand | flags::kDeviceRightCommand;

constexpr ModifierKey kModifierKeys[] = {
    {keycodes::kShift, flags::kDeviceLeftShift, kShiftPair, flags::kShift},
    {keycodes::kRightShift, flags::kDeviceRightShift, kShiftPair, flags::kShift},
    {keycodes::kControl, flags::kDeviceLeftControl, kControlPair, flags::kControl},
    {keycodes::kRightControl, flags::kDeviceRightControl, kControlPair, flags::kControl},
    {keycodes::kOption, flags::kDeviceLeftAlternate, kAlternatePair, flags::kAlternate},
    {keycodes::kRightOption, flags::kDeviceRightAlternate, kAlternatePair, flags::kAlternate},
    {keycodes::kCommand, flags::kDeviceLeftCommand, kCommandPair, flags::kCommand},
    {keycodes::kRightCommand, flags::kDeviceRightCommand, kCommandPair, flags::kCommand},
    {keycodes::kFunction, 0, 0, flags::kSecondaryFn},
};

const ModifierKey* find_modifier(std::uint16_t keycode) noexcept {
    for (const ModifierKey& m : kModifierKeys) {
        if (m.keycode == keycode) {
            return &m;
        }
    }
    return nullptr;
}

}  // namespace

ModifierState modifiers_from_flags(std::uint64_t f) noexcept {
    ModifierState m;
    auto side = [&](std::uint64_t left_bit, std::uint64_t right_bit, std::uint64_t class_bit, Modifier left,
                    Modifier right) {
        const bool any_device_bit = (f & (left_bit | right_bit)) != 0;
        if (any_device_bit) {
            m.set(left, (f & left_bit) != 0);
            m.set(right, (f & right_bit) != 0);
        } else if ((f & class_bit) != 0) {
            m.set(left);  // the source does not report sides
        }
    };
    side(flags::kDeviceLeftShift, flags::kDeviceRightShift, flags::kShift, Modifier::LeftShift, Modifier::RightShift);
    side(flags::kDeviceLeftControl, flags::kDeviceRightControl, flags::kControl, Modifier::LeftControl,
         Modifier::RightControl);
    side(flags::kDeviceLeftAlternate, flags::kDeviceRightAlternate, flags::kAlternate, Modifier::LeftAlt,
         Modifier::RightAlt);
    side(flags::kDeviceLeftCommand, flags::kDeviceRightCommand, flags::kCommand, Modifier::LeftMeta,
         Modifier::RightMeta);
    m.set(Modifier::CapsLock, (f & flags::kAlphaShift) != 0);
    m.set(Modifier::Function, (f & flags::kSecondaryFn) != 0);
    return m;
}

std::optional<bool> modifier_is_down(std::uint16_t keycode, std::uint64_t f) noexcept {
    const ModifierKey* key = find_modifier(keycode);
    if (key == nullptr) {
        return std::nullopt;
    }
    if (key->device_mask == 0) {
        return (f & key->class_mask) != 0;
    }
    if ((f & key->pair_mask) != 0) {
        return (f & key->device_mask) != 0;
    }
    // No side information at all: fall back to the class bit.
    return (f & key->class_mask) != 0;
}

std::optional<KeyEvent> to_key_event(NativeType type, std::uint16_t keycode, std::uint64_t f, bool autorepeat,
                                     Timestamp timestamp) noexcept {
    KeyEvent event;
    event.code = keycode;
    event.timestamp = timestamp;
    event.modifiers = modifiers_from_flags(f);
    switch (type) {
        case NativeType::KeyDown:
            event.action = autorepeat ? KeyAction::Repeat : KeyAction::Down;
            break;
        case NativeType::KeyUp:
            event.action = KeyAction::Up;
            break;
        case NativeType::FlagsChanged: {
            const std::optional<bool> down = modifier_is_down(keycode, f);
            if (!down) {
                return std::nullopt;
            }
            event.action = *down ? KeyAction::Down : KeyAction::Up;
            event.role = KeyRole::Modifier;
            break;
        }
        default:
            return std::nullopt;
    }
    return event;
}

std::uint64_t ticks_to_ns(std::uint64_t ticks, std::uint32_t numer, std::uint32_t denom) noexcept {
    if (numer == denom || denom == 0) {
        return ticks;
    }
    // Split to avoid overflowing 64 bits for large tick counts.
    const std::uint64_t whole = ticks / denom;
    const std::uint64_t rest = ticks % denom;
    return whole * numer + rest * numer / denom;
}

Timestamp event_timestamp_to_ns(std::uint64_t event_timestamp, std::uint64_t now_ticks, std::uint32_t numer,
                                std::uint32_t denom) noexcept {
    const std::uint64_t now_ns = ticks_to_ns(now_ticks, numer, denom);
    if (numer == denom || denom == 0) {
        return Timestamp(static_cast<std::int64_t>(event_timestamp));
    }
    auto distance = [](std::uint64_t a, std::uint64_t b) { return a > b ? a - b : b - a; };
    const std::uint64_t as_ns = event_timestamp;
    const std::uint64_t as_ticks_in_ns = ticks_to_ns(event_timestamp, numer, denom);
    const std::uint64_t chosen = distance(as_ns, now_ns) <= distance(as_ticks_in_ns, now_ns) ? as_ns : as_ticks_in_ns;
    return Timestamp(static_cast<std::int64_t>(chosen));
}

ResolvedTimestamp resolve_event_timestamp(std::uint64_t event_timestamp, std::uint64_t now_ticks, std::uint32_t numer,
                                          std::uint32_t denom) noexcept {
    const Timestamp now(static_cast<std::int64_t>(ticks_to_ns(now_ticks, numer, denom)));
    if (event_timestamp == 0) {
        return {now, TimestampSource::Arrival};
    }
    const Timestamp candidate = event_timestamp_to_ns(event_timestamp, now_ticks, numer, denom);
    const Duration age = now - candidate;
    if (age < -Duration(std::chrono::milliseconds(1)) || age > Duration(kMaxEventAge)) {
        return {now, TimestampSource::Arrival};
    }
    const bool as_ticks = numer != denom && denom != 0 && candidate.count() != static_cast<std::int64_t>(event_timestamp);
    return {candidate, as_ticks ? TimestampSource::EventTicks : TimestampSource::EventNanoseconds};
}

const char* to_string(TimestampSource source) noexcept {
    switch (source) {
        case TimestampSource::EventNanoseconds: return "event timestamps (nanoseconds)";
        case TimestampSource::EventTicks: return "event timestamps (mach ticks)";
        case TimestampSource::Arrival: return "arrival time (event timestamps unusable)";
    }
    return "unknown";
}

}  // namespace kcf::macos
