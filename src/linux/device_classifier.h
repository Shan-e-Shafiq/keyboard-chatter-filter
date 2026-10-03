// SPDX-License-Identifier: MIT
//
// Decides which evdev devices are keyboards the filter should take over. Pure logic over capability
// bitmaps, free of Linux headers so it is unit-tested on every CI platform. Constants mirror
// <linux/input-event-codes.h> and <linux/input.h> (stable kernel ABI; input_device.cpp checks them
// against the real headers at compile time).
#pragma once

#include <bitset>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace kcf::linux_input {

namespace ev {
inline constexpr unsigned kSyn = 0x00;
inline constexpr unsigned kKey = 0x01;
inline constexpr unsigned kRel = 0x02;
inline constexpr unsigned kAbs = 0x03;
inline constexpr unsigned kMsc = 0x04;
inline constexpr unsigned kLed = 0x11;
inline constexpr unsigned kRep = 0x14;
inline constexpr unsigned kCount = 0x20;
}  // namespace ev

namespace code {
inline constexpr unsigned kKeyEnter = 28;
inline constexpr unsigned kKeySpace = 57;
inline constexpr unsigned kKeyKp7 = 71;
inline constexpr unsigned kKeyKp0 = 82;
inline constexpr unsigned kKeyKpEnter = 96;
inline constexpr unsigned kBtnMisc = 0x100;
inline constexpr unsigned kBtnLeft = 0x110;
inline constexpr unsigned kBtnJoystick = 0x120;
inline constexpr unsigned kBtnGamepad = 0x130;
inline constexpr unsigned kBtnToolPen = 0x140;
inline constexpr unsigned kBtnToolFinger = 0x145;
inline constexpr unsigned kBtnTouch = 0x14a;
inline constexpr unsigned kBtnStylus = 0x14b;
inline constexpr unsigned kKeyCount = 0x300;
inline constexpr unsigned kRelX = 0x00;
inline constexpr unsigned kRelY = 0x01;
inline constexpr unsigned kRelCount = 0x10;
inline constexpr unsigned kAbsX = 0x00;
inline constexpr unsigned kAbsY = 0x01;
inline constexpr unsigned kAbsMtPositionX = 0x35;
inline constexpr unsigned kAbsCount = 0x40;
inline constexpr unsigned kMscScan = 0x04;
inline constexpr unsigned kMscCount = 0x08;
inline constexpr unsigned kLedCount = 0x10;
}  // namespace code

namespace bus {
inline constexpr std::uint16_t kUsb = 0x03;
inline constexpr std::uint16_t kBluetooth = 0x05;
inline constexpr std::uint16_t kVirtual = 0x06;
inline constexpr std::uint16_t kI8042 = 0x11;
}  // namespace bus

inline constexpr std::uint16_t kYubicoVendorId = 0x1050;

// Every virtual keyboard we create is named with this prefix, so we never grab our own output.
inline constexpr std::string_view kVirtualDevicePrefix = "keyboard-chatter-filter: ";

struct DeviceCapabilities {
    std::string name;
    std::uint16_t bustype = 0;
    std::uint16_t vendor = 0;
    std::uint16_t product = 0;
    std::bitset<ev::kCount> events;
    std::bitset<code::kKeyCount> keys;
    std::bitset<code::kRelCount> rel;
    std::bitset<code::kAbsCount> abs;
    std::bitset<code::kMscCount> msc;
    std::bitset<code::kLedCount> leds;
};

enum class DeviceKind {
    Keyboard,     // filter it
    NotKeyboard,  // mouse, touchpad, power button, ...: leave alone
    Ignored,      // looks like a keyboard but must not be filtered
};

struct Classification {
    DeviceKind kind = DeviceKind::NotKeyboard;
    std::string reason;
};

[[nodiscard]] Classification classify_device(const DeviceCapabilities& caps,
                                             const std::vector<std::string>& ignored_names);

}  // namespace kcf::linux_input
