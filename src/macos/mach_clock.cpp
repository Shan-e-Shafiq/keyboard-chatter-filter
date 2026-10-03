// SPDX-License-Identifier: MIT
#include "macos/mach_clock.h"

#include <mach/mach_time.h>

#include "macos/event_conversion.h"

namespace kcf::macos {

MachClock::MachClock() {
    mach_timebase_info_data_t info{};
    if (mach_timebase_info(&info) == KERN_SUCCESS && info.denom != 0) {
        numer_ = info.numer;
        denom_ = info.denom;
    }
}

Timestamp MachClock::now() const noexcept {
    return Timestamp(static_cast<std::int64_t>(ticks_to_ns(mach_absolute_time(), numer_, denom_)));
}

Timestamp MachClock::from_event_timestamp(std::uint64_t event_timestamp) const noexcept {
    return event_timestamp_to_ns(event_timestamp, mach_absolute_time(), numer_, denom_);
}

ResolvedTimestamp MachClock::resolve(std::uint64_t event_timestamp) const noexcept {
    return resolve_event_timestamp(event_timestamp, mach_absolute_time(), numer_, denom_);
}

}  // namespace kcf::macos
