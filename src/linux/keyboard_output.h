// SPDX-License-Identifier: MIT
#pragma once

#include <linux/input.h>

#include <cstdint>
#include <vector>

#include "keyboard_filter/keyboard_io.h"
#include "linux/input_device.h"

namespace kcf::linux_input {

// Writes to the virtual keyboard (uinput) applications read from. Events are grouped into the same
// SYN_REPORT frames the physical keyboard produced; events emitted outside a frame (held-back
// releases going out on a timer) get a frame of their own.
class UinputKeyboardOutput final : public IKeyboardOutput {
public:
    explicit UinputKeyboardOutput(UinputDevice device);

    // IKeyboardOutput: a key event chosen by the filter (deferred release, or a resync).
    void emit(const KeyEvent& event) override;

    // Accepted raw events from the physical device, in order.
    void forward(const input_event& event);

    void begin_frame() noexcept { frame_open_ = true; }
    void end_frame();      // writes the frame followed by SYN_REPORT (nothing if empty)
    void discard_frame();  // after SYN_DROPPED: the partial frame is unreliable
    [[nodiscard]] bool frame_open() const noexcept { return frame_open_; }

    [[nodiscard]] UinputDevice& device() noexcept { return device_; }
    [[nodiscard]] std::uint64_t write_failures() const noexcept { return write_failures_; }

private:
    void write_now(std::vector<input_event>& events);

    UinputDevice device_;
    std::vector<input_event> frame_;
    std::vector<input_event> scratch_;
    bool frame_open_ = false;
    std::uint64_t write_failures_ = 0;
};

[[nodiscard]] input_event make_input_event(unsigned type, unsigned code, int value) noexcept;

}  // namespace kcf::linux_input
