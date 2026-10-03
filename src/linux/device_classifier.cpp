// SPDX-License-Identifier: MIT
#include "linux/device_classifier.h"

#include "common/text.h"

namespace kcf::linux_input {
namespace {

// KEY_Q..KEY_P, KEY_A..KEY_L, KEY_Z..KEY_M: the 26 letter keys.
constexpr unsigned kLetterKeys[] = {16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 30, 31, 32, 33,
                                    34, 35, 36, 37, 38, 44, 45, 46, 47, 48, 49, 50};

bool has_typing_keys(const DeviceCapabilities& c) {
    int letters = 0;
    for (const unsigned k : kLetterKeys) {
        letters += c.keys.test(k) ? 1 : 0;
    }
    return letters >= 20 && c.keys.test(code::kKeySpace) && c.keys.test(code::kKeyEnter);
}

bool is_keypad(const DeviceCapabilities& c) {
    for (unsigned k = code::kKeyKp7; k <= code::kKeyKp0; ++k) {  // KEY_KP7 (71) .. KEY_KP0 (82)
        if (!c.keys.test(k)) {
            return false;
        }
    }
    return c.keys.test(code::kKeyKpEnter);
}

bool is_pointer(const DeviceCapabilities& c) {
    if (c.keys.test(code::kBtnLeft) || c.keys.test(code::kBtnTouch) || c.keys.test(code::kBtnToolFinger) ||
        c.keys.test(code::kBtnToolPen) || c.keys.test(code::kBtnStylus)) {
        return true;
    }
    if (c.events.test(ev::kRel) && (c.rel.test(code::kRelX) || c.rel.test(code::kRelY))) {
        return true;
    }
    return c.events.test(ev::kAbs) &&
           (c.abs.test(code::kAbsX) || c.abs.test(code::kAbsY) || c.abs.test(code::kAbsMtPositionX));
}

bool is_game_controller(const DeviceCapabilities& c) {
    for (unsigned k = code::kBtnJoystick; k < code::kBtnJoystick + 0x20; ++k) {  // BTN_JOYSTICK..BTN_GAMEPAD range
        if (c.keys.test(k)) {
            return true;
        }
    }
    return false;
}

}  // namespace

Classification classify_device(const DeviceCapabilities& c, const std::vector<std::string>& ignored_names) {
    if (std::string_view(c.name).substr(0, kVirtualDevicePrefix.size()) == kVirtualDevicePrefix) {
        return {DeviceKind::Ignored, "our own filtered keyboard"};
    }
    if (!c.events.test(ev::kKey)) {
        return {DeviceKind::NotKeyboard, "no keys"};
    }
    if (is_pointer(c)) {
        return {DeviceKind::NotKeyboard, "pointing device"};
    }
    if (is_game_controller(c)) {
        return {DeviceKind::NotKeyboard, "game controller"};
    }
    if (!has_typing_keys(c) && !is_keypad(c)) {
        return {DeviceKind::NotKeyboard, "no typing keys"};
    }
    if (c.bustype == bus::kVirtual) {
        // Software-generated input (ydotool, remote desktop, other remappers' output).
        return {DeviceKind::Ignored, "virtual keyboard (software input is never filtered)"};
    }
    if (c.vendor == kYubicoVendorId) {
        return {DeviceKind::Ignored, "YubiKey (types one-time passwords faster than a human; never filtered)"};
    }
    for (const std::string& pattern : ignored_names) {
        if (text::icontains(c.name, pattern)) {
            return {DeviceKind::Ignored, "matches ignored_devices entry \"" + pattern + "\""};
        }
    }
    return {DeviceKind::Keyboard, is_keypad(c) && !has_typing_keys(c) ? "keypad" : "keyboard"};
}

}  // namespace kcf::linux_input
