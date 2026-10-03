// SPDX-License-Identifier: MIT
//
// Translation rules of the platform adapters, tested on every platform.

#include "linux/device_classifier.h"
#include "macos/event_conversion.h"
#include "test_framework.h"
#include "windows/key_mapping.h"

using namespace kcf;
using namespace std::chrono_literals;

namespace kcf {
inline std::ostream& operator<<(std::ostream& os, KeyAction a) { return os << to_string(a); }
}  // namespace kcf

// ---------------------------------------------------------------------------------------------
// macOS
// ---------------------------------------------------------------------------------------------

namespace mac = kcf::macos;

TEST(MacOS, KeyDownUpAndRepeat) {
    const auto down = mac::to_key_event(mac::NativeType::KeyDown, 0x00, 0, false, Timestamp(5));
    ASSERT_TRUE(down.has_value());
    EXPECT_EQ(down->code, 0x00);
    EXPECT_EQ(down->action, KeyAction::Down);
    EXPECT_EQ(down->timestamp, Timestamp(5));
    EXPECT_FALSE(down->is_modifier());

    const auto repeat = mac::to_key_event(mac::NativeType::KeyDown, 0x00, 0, true, Timestamp(6));
    ASSERT_TRUE(repeat.has_value());
    EXPECT_EQ(repeat->action, KeyAction::Repeat);

    const auto up = mac::to_key_event(mac::NativeType::KeyUp, 0x00, 0, false, Timestamp(7));
    ASSERT_TRUE(up.has_value());
    EXPECT_EQ(up->action, KeyAction::Up);
}

TEST(MacOS, LeftAndRightModifiersAreDistinguished) {
    using namespace mac::flags;
    // Right shift pressed while left shift is already down.
    const std::uint64_t both = kShift | kDeviceLeftShift | kDeviceRightShift;
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kRightShift, both), std::optional<bool>(true));
    // Left shift released, right still held: class bit stays set but the left device bit is gone.
    const std::uint64_t right_only = kShift | kDeviceRightShift;
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kShift, right_only), std::optional<bool>(false));
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kRightShift, right_only), std::optional<bool>(true));

    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kCommand, kCommand | kDeviceLeftCommand), std::optional<bool>(true));
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kRightOption, kAlternate | kDeviceRightAlternate),
              std::optional<bool>(true));
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kRightControl, kControl | kDeviceRightControl),
              std::optional<bool>(true));
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kControl, 0), std::optional<bool>(false));
}

TEST(MacOS, ModifierWithoutSideInformationFallsBackToClassBit) {
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kShift, mac::flags::kShift), std::optional<bool>(true));
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kShift, 0), std::optional<bool>(false));
}

TEST(MacOS, FunctionKey) {
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kFunction, mac::flags::kSecondaryFn), std::optional<bool>(true));
    EXPECT_EQ(mac::modifier_is_down(mac::keycodes::kFunction, 0), std::optional<bool>(false));
}

TEST(MacOS, CapsLockAndUnknownFlagsChangedPassThrough) {
    EXPECT_FALSE(mac::to_key_event(mac::NativeType::FlagsChanged, mac::keycodes::kCapsLock, mac::flags::kAlphaShift,
                                   false, Timestamp(0))
                     .has_value());
    EXPECT_FALSE(mac::to_key_event(mac::NativeType::FlagsChanged, 0x00, 0, false, Timestamp(0)).has_value());
}

TEST(MacOS, FlagsChangedBecomesModifierKeyEvent) {
    using namespace mac::flags;
    const auto down = mac::to_key_event(mac::NativeType::FlagsChanged, mac::keycodes::kShift, kShift | kDeviceLeftShift,
                                        false, Timestamp(1));
    ASSERT_TRUE(down.has_value());
    EXPECT_EQ(down->action, KeyAction::Down);
    EXPECT_TRUE(down->is_modifier());
    EXPECT_TRUE(down->modifiers.has(Modifier::LeftShift));

    const auto up = mac::to_key_event(mac::NativeType::FlagsChanged, mac::keycodes::kShift, 0, false, Timestamp(2));
    ASSERT_TRUE(up.has_value());
    EXPECT_EQ(up->action, KeyAction::Up);
}

TEST(MacOS, ModifierSnapshot) {
    using namespace mac::flags;
    const ModifierState m = mac::modifiers_from_flags(kShift | kDeviceRightShift | kCommand | kDeviceLeftCommand |
                                                      kAlphaShift | kSecondaryFn);
    EXPECT_TRUE(m.has(Modifier::RightShift));
    EXPECT_FALSE(m.has(Modifier::LeftShift));
    EXPECT_TRUE(m.has(Modifier::LeftMeta));
    EXPECT_TRUE(m.has(Modifier::CapsLock));
    EXPECT_TRUE(m.has(Modifier::Function));
    EXPECT_FALSE(m.has(Modifier::LeftControl));
}

TEST(MacOS, TimestampsOnIntelAreNanoseconds) {
    EXPECT_EQ(mac::event_timestamp_to_ns(123'456'789, 123'999'999, 1, 1), Timestamp(123'456'789));
}

TEST(MacOS, TimestampsOnAppleSiliconAreDetected) {
    // Timebase 125/3: one tick is 41.67 ns.
    const std::uint64_t now_ticks = 3'600ULL * 24'000'000ULL;  // one hour after boot
    const std::uint64_t now_ns = mac::ticks_to_ns(now_ticks, 125, 3);
    // Reported in nanoseconds, 5 ms ago.
    EXPECT_EQ(mac::event_timestamp_to_ns(now_ns - 5'000'000, now_ticks, 125, 3), Timestamp(now_ns - 5'000'000));
    // Reported in ticks, 5 ms ago (120000 ticks).
    EXPECT_EQ(mac::event_timestamp_to_ns(now_ticks - 120'000, now_ticks, 125, 3), Timestamp(now_ns - 5'000'000));
}

TEST(MacOS, TickConversionDoesNotOverflow) {
    const std::uint64_t ticks = 1ULL << 60;
    EXPECT_EQ(mac::ticks_to_ns(ticks, 125, 3), (ticks / 3) * 125 + (ticks % 3) * 125 / 3);
}

TEST(MacOS, ZeroTimestampFallsBackToArrivalTime) {
    // Synthetic and some tapped events carry timestamp 0. Trusting it would make every re-press of
    // a key look like a 0 ms bounce (each key would work only once).
    const std::uint64_t now_ticks = 240'000'000'000ULL;
    const auto r = mac::resolve_event_timestamp(0, now_ticks, 125, 3);
    EXPECT_TRUE(r.source == mac::TimestampSource::Arrival);
    EXPECT_EQ(r.time, Timestamp(static_cast<std::int64_t>(mac::ticks_to_ns(now_ticks, 125, 3))));
}

TEST(MacOS, ImplausibleTimestampsFallBackToArrivalTime) {
    const std::uint64_t now_ticks = 240'000'000'000ULL;
    const std::uint64_t now_ns = mac::ticks_to_ns(now_ticks, 125, 3);
    // From the future, ancient, and garbage.
    for (const std::uint64_t ts : {now_ns + 50'000'000ULL, now_ns - 10'000'000'000ULL, 12345ULL, ~0ULL}) {
        EXPECT_TRUE(mac::resolve_event_timestamp(ts, now_ticks, 125, 3).source == mac::TimestampSource::Arrival);
    }
}

TEST(MacOS, PlausibleTimestampsAreUsed) {
    const std::uint64_t now_ticks = 240'000'000'000ULL;
    const std::uint64_t now_ns = mac::ticks_to_ns(now_ticks, 125, 3);
    const auto ns = mac::resolve_event_timestamp(now_ns - 3'000'000, now_ticks, 125, 3);
    EXPECT_TRUE(ns.source == mac::TimestampSource::EventNanoseconds);
    EXPECT_EQ(ns.time, Timestamp(static_cast<std::int64_t>(now_ns - 3'000'000)));
    const auto ticks = mac::resolve_event_timestamp(now_ticks - 72'000, now_ticks, 125, 3);  // 3 ms ago
    EXPECT_TRUE(ticks.source == mac::TimestampSource::EventTicks);
    EXPECT_EQ(ticks.time, Timestamp(static_cast<std::int64_t>(now_ns - 3'000'000)));
    // Intel: ticks are nanoseconds.
    EXPECT_TRUE(mac::resolve_event_timestamp(9'000'000'000ULL, 9'001'000'000ULL, 1, 1).source ==
                mac::TimestampSource::EventNanoseconds);
}

// ---------------------------------------------------------------------------------------------
// Windows
// ---------------------------------------------------------------------------------------------

namespace win = kcf::windows;

TEST(Windows, ScanCodeIdentifiesPhysicalKey) {
    const auto a = win::physical_key_code({0x41, 0x1E, 0});
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(*a, 0x1E);
    // Left and right Control share scan code 0x1D; the extended flag tells them apart.
    const auto lctrl = win::physical_key_code({win::vk::kLControl, 0x1D, 0});
    const auto rctrl = win::physical_key_code({win::vk::kRControl, 0x1D, win::llkhf::kExtended});
    ASSERT_TRUE(lctrl && rctrl);
    EXPECT_NE(*lctrl, *rctrl);
    EXPECT_EQ(*rctrl, 0x11D);
}

TEST(Windows, LayoutIndependentIdentity) {
    // The same physical key produces different virtual keys on different layouts; the code follows
    // the scan code.
    EXPECT_EQ(win::physical_key_code({0x51, 0x10, 0}), win::physical_key_code({0x41, 0x10, 0}));
}

TEST(Windows, InjectedEventsAreNotFiltered) {
    EXPECT_FALSE(win::to_key_event({0x41, 0x1E, win::llkhf::kInjected}, Timestamp(0), {}).has_value());
    EXPECT_FALSE(win::to_key_event({0x41, 0x1E, win::llkhf::kInjected | win::llkhf::kLowerIlInjected}, Timestamp(0), {})
                     .has_value());
}

TEST(Windows, DriverArtefactsPassThrough) {
    EXPECT_FALSE(win::physical_key_code({win::vk::kLControl, 0x21D, 0}).has_value());              // AltGr's Control
    EXPECT_FALSE(win::physical_key_code({win::vk::kLShift, 0x2A, win::llkhf::kExtended}).has_value());  // fake shift
    EXPECT_FALSE(win::physical_key_code({win::vk::kPacket, 0, 0}).has_value());                     // Unicode injection
    EXPECT_FALSE(win::physical_key_code({0, 0, 0}).has_value());
}

TEST(Windows, MissingScanCodeFallsBackToVirtualKey) {
    const auto code = win::physical_key_code({0xB3, 0, 0});  // media key without a scan code
    ASSERT_TRUE(code.has_value());
    EXPECT_EQ(*code, 0x4B3);
    EXPECT_LT(*code, static_cast<KeyCode>(kKeyCodeLimit));
}

TEST(Windows, UpFlagAndModifierRole) {
    const auto up = win::to_key_event({win::vk::kLShift, 0x2A, win::llkhf::kUp}, Timestamp(9), {});
    ASSERT_TRUE(up.has_value());
    EXPECT_EQ(up->action, KeyAction::Up);
    EXPECT_TRUE(up->is_modifier());
    const auto down = win::to_key_event({0x41, 0x1E, 0}, Timestamp(9), {});
    ASSERT_TRUE(down.has_value());
    EXPECT_EQ(down->action, KeyAction::Down);
    EXPECT_FALSE(down->is_modifier());
}

TEST(Windows, ModifierTracking) {
    ModifierState m;
    win::update_modifiers(m, win::vk::kLShift, true);
    win::update_modifiers(m, win::vk::kRMenu, true);
    EXPECT_TRUE(m.has(Modifier::LeftShift));
    EXPECT_TRUE(m.has(Modifier::RightAlt));
    win::update_modifiers(m, win::vk::kLShift, false);
    EXPECT_FALSE(m.has(Modifier::LeftShift));
    win::update_modifiers(m, 0x41, true);
    EXPECT_EQ(m.bits(), ModifierState{}.set(Modifier::RightAlt).bits());
}

TEST(Windows, InjectionReproducesTheKey) {
    const auto spec = win::injection_for({win::vk::kRControl, 0x1D, win::llkhf::kExtended | win::llkhf::kUp}, true);
    EXPECT_EQ(spec.vk, win::vk::kRControl);
    EXPECT_EQ(spec.scan, 0x1D);
    EXPECT_EQ(spec.flags, win::keyeventf::kExtendedKey | win::keyeventf::kKeyUp);
    const auto down = win::injection_for({0x41, 0x1E, 0}, false);
    EXPECT_EQ(down.flags, 0u);
}

// ---------------------------------------------------------------------------------------------
// Linux device classification
// ---------------------------------------------------------------------------------------------

namespace li = kcf::linux_input;

namespace {

li::DeviceCapabilities full_keyboard(const std::string& name = "Logitech K380") {
    li::DeviceCapabilities c;
    c.name = name;
    c.bustype = li::bus::kBluetooth;
    c.vendor = 0x046d;
    c.events.set(li::ev::kKey).set(li::ev::kMsc).set(li::ev::kLed).set(li::ev::kRep);
    for (unsigned k = 1; k <= 88; ++k) {
        c.keys.set(k);
    }
    return c;
}

}  // namespace

TEST(Linux, RegularKeyboardIsFiltered) {
    EXPECT_EQ(li::classify_device(full_keyboard(), {}).kind, li::DeviceKind::Keyboard);
    auto internal = full_keyboard("AT Translated Set 2 keyboard");
    internal.bustype = li::bus::kI8042;
    EXPECT_EQ(li::classify_device(internal, {}).kind, li::DeviceKind::Keyboard);
}

TEST(Linux, MiceAndTouchpadsAreLeftAlone) {
    li::DeviceCapabilities mouse;
    mouse.name = "USB Optical Mouse";
    mouse.events.set(li::ev::kKey).set(li::ev::kRel);
    mouse.keys.set(li::code::kBtnLeft).set(li::code::kBtnLeft + 1);
    mouse.rel.set(li::code::kRelX).set(li::code::kRelY);
    EXPECT_EQ(li::classify_device(mouse, {}).kind, li::DeviceKind::NotKeyboard);

    li::DeviceCapabilities touchpad;
    touchpad.name = "SynPS/2 Synaptics TouchPad";
    touchpad.events.set(li::ev::kKey).set(li::ev::kAbs);
    touchpad.keys.set(li::code::kBtnTouch).set(li::code::kBtnToolFinger);
    touchpad.abs.set(li::code::kAbsX).set(li::code::kAbsY);
    EXPECT_EQ(li::classify_device(touchpad, {}).kind, li::DeviceKind::NotKeyboard);
}

TEST(Linux, KeyboardWithBuiltInPointerIsLeftAlone) {
    auto combo = full_keyboard("Logitech K400");
    combo.events.set(li::ev::kRel);
    combo.rel.set(li::code::kRelX).set(li::code::kRelY);
    combo.keys.set(li::code::kBtnLeft);
    EXPECT_EQ(li::classify_device(combo, {}).kind, li::DeviceKind::NotKeyboard);
}

TEST(Linux, KeyboardWithVolumeWheelIsFiltered) {
    auto keyboard = full_keyboard();
    keyboard.events.set(li::ev::kRel);
    keyboard.rel.set(0x06);  // REL_HWHEEL / dial-type axes are fine
    EXPECT_EQ(li::classify_device(keyboard, {}).kind, li::DeviceKind::Keyboard);
}

TEST(Linux, ButtonOnlyDevicesAreNotKeyboards) {
    li::DeviceCapabilities power;
    power.name = "Power Button";
    power.events.set(li::ev::kKey);
    power.keys.set(116);  // KEY_POWER
    EXPECT_EQ(li::classify_device(power, {}).kind, li::DeviceKind::NotKeyboard);

    li::DeviceCapabilities nothing;
    EXPECT_EQ(li::classify_device(nothing, {}).kind, li::DeviceKind::NotKeyboard);
}

TEST(Linux, GameControllersAreNotKeyboards) {
    auto pad = full_keyboard("Gamepad with keys");
    pad.keys.set(li::code::kBtnGamepad);
    EXPECT_EQ(li::classify_device(pad, {}).kind, li::DeviceKind::NotKeyboard);
}

TEST(Linux, NumericKeypadIsFiltered) {
    li::DeviceCapabilities keypad;
    keypad.name = "USB Numeric Keypad";
    keypad.events.set(li::ev::kKey);
    for (unsigned k = li::code::kKeyKp7; k <= li::code::kKeyKp0; ++k) {
        keypad.keys.set(k);
    }
    keypad.keys.set(li::code::kKeyKpEnter);
    EXPECT_EQ(li::classify_device(keypad, {}).kind, li::DeviceKind::Keyboard);
}

TEST(Linux, OwnVirtualKeyboardIsNeverGrabbed) {
    auto own = full_keyboard(std::string(li::kVirtualDevicePrefix) + "Logitech K380");
    own.bustype = li::bus::kVirtual;
    EXPECT_EQ(li::classify_device(own, {}).kind, li::DeviceKind::Ignored);
}

TEST(Linux, SoftwareKeyboardsAreIgnored) {
    auto ydotool = full_keyboard("ydotoold virtual device");
    ydotool.bustype = li::bus::kVirtual;
    EXPECT_EQ(li::classify_device(ydotool, {}).kind, li::DeviceKind::Ignored);
}

TEST(Linux, YubiKeyIsIgnored) {
    auto yubikey = full_keyboard("Yubico YubiKey OTP+FIDO+CCID");
    yubikey.bustype = li::bus::kUsb;
    yubikey.vendor = li::kYubicoVendorId;
    EXPECT_EQ(li::classify_device(yubikey, {}).kind, li::DeviceKind::Ignored);
}

TEST(Linux, ConfiguredIgnoredDevices) {
    const std::vector<std::string> ignored{"barcode"};
    EXPECT_EQ(li::classify_device(full_keyboard("Honeywell Barcode Scanner"), ignored).kind, li::DeviceKind::Ignored);
    EXPECT_EQ(li::classify_device(full_keyboard("Keychron K2"), ignored).kind, li::DeviceKind::Keyboard);
}

KCF_TEST_MAIN()
