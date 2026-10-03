// SPDX-License-Identifier: MIT
#pragma once

#include <cstdint>

#include "keyboard_filter/keyboard_io.h"

namespace kcf::macos {

// mach_absolute_time() in nanoseconds: the clock CGEvent timestamps are based on. It does not
// advance while the machine sleeps, which is exactly what chatter timing needs.
class MachClock final : public IClock {
public:
    MachClock();
    [[nodiscard]] Timestamp now() const noexcept override;
    [[nodiscard]] Timestamp from_event_timestamp(std::uint64_t event_timestamp) const noexcept;

private:
    std::uint32_t numer_ = 1;
    std::uint32_t denom_ = 1;
};

}  // namespace kcf::macos
