// SPDX-License-Identifier: MIT
#pragma once

#include <linux/input.h>

#include <functional>
#include <optional>
#include <string>

#include "keyboard_filter/keyboard_io.h"
#include "linux/input_device.h"
#include "linux/keyboard_output.h"

namespace kcf::linux_input {

// Takes exclusive ownership of one physical keyboard (EVIOCGRAB) and routes its events through the
// filter into its virtual twin:
//
//   physical keyboard -> evdev (grabbed) -> filter -> uinput virtual keyboard -> applications
//
// Because the physical device is grabbed, applications only ever receive the filtered stream, never
// both. The grab is taken only while no key is held (so nothing is left stuck in the applications
// that saw the press), and it disappears automatically if this process exits for any reason.
class EvdevKeyboard final : public IKeyboardInterceptor {
public:
    // `output` null = dry run: the device is not grabbed, its events are only observed and counted.
    EvdevKeyboard(EvdevDevice device, UinputKeyboardOutput* output, DeviceId id,
                  std::function<void()> before_key_event);
    ~EvdevKeyboard() override;
    EvdevKeyboard(const EvdevKeyboard&) = delete;
    EvdevKeyboard& operator=(const EvdevKeyboard&) = delete;

    InterceptorStartResult start(IKeyEventHandler& handler) override;
    void stop() noexcept override;
    [[nodiscard]] bool is_active() const noexcept override {
        return device_.grabbed() || (output_ == nullptr && handler_ != nullptr);
    }
    [[nodiscard]] bool waiting_for_release() const noexcept { return waiting_for_release_; }

    enum class ReadStatus { Ok, Gone };
    // Drains the device. Gone means it was unplugged (or failed) and must be discarded.
    [[nodiscard]] ReadStatus on_readable();

    // Applications set keyboard LEDs (Caps Lock, ...) on the virtual keyboard; mirror them on the
    // physical one.
    void on_virtual_led_feedback();

    [[nodiscard]] const EvdevDevice& device() const noexcept { return device_; }

private:
    void handle(const input_event& ev);
    void handle_key(const input_event& ev);
    void resync();
    bool try_grab(std::string& error);

    EvdevDevice device_;
    UinputKeyboardOutput* output_;
    DeviceId id_;
    std::function<void()> before_key_event_;
    IKeyEventHandler* handler_ = nullptr;
    ModifierState modifiers_;
    std::optional<input_event> pending_scan_;
    bool waiting_for_release_ = false;
    bool dropping_ = false;
};

[[nodiscard]] Timestamp event_time(const input_event& ev) noexcept;
[[nodiscard]] bool is_modifier_key(unsigned code) noexcept;

}  // namespace kcf::linux_input
