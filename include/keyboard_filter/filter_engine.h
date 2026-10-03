// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <vector>

#include "keyboard_filter/chatter_filter.h"
#include "keyboard_filter/keyboard_io.h"

namespace kcf {

// Glue between an interceptor, the chatter filter, the output layer and the platform's timer.
// Identical on every platform; adapters only provide the four interfaces it talks to.
//
// It also contains the safety circuit breaker. Real switch chatter only ever removes the extra
// transitions inside a burst lasting a few milliseconds; the next deliberate press of the key always
// gets through. If the filter instead keeps dropping a key's presses over a longer period, its
// decisions are wrong (for example because the platform delivers unusable timestamps). The engine then stops filtering altogether and lets every event through
// unchanged, so a fault can never take the keyboard away from the user.
class FilterEngine final : public IKeyEventHandler {
public:
    // Real chatter drops come in bursts milliseconds apart, and the next deliberate press of the key
    // gets through. A *slow* drop is one that comes at least max(kMinSlowDropGap, 2 x threshold)
    // after the key's previous drop, i.e. looks like a separate deliberate press. This many slow drops
    // of one key with no accepted press in between trips the breaker.
    static constexpr int kRunawaySlowDrops = 4;
    static constexpr std::chrono::milliseconds kMinSlowDropGap{50};

    FilterEngine(FilterSettings settings, IKeyboardOutput& output, IWakeupScheduler& scheduler,
                 const IClock& clock);

    Decision on_key_event(const KeyEvent& event) override;
    void on_events_lost() override;
    void on_key_state(KeyCode code, bool physically_down, Timestamp now) override {
        resync_key(code, physically_down, now);
    }

    // The platform timer requested through IWakeupScheduler fired.
    void on_wakeup();

    // Delivers every held-back release now (shutdown, stopping interception, cross-device ordering).
    void flush();

    // Applies new settings and re-arms a tripped circuit breaker.
    void update_settings(const FilterSettings& settings);

    // Reconciles one key with its authoritative physical state (see ChatterFilter::resync_key).
    void resync_key(KeyCode code, bool physically_down, Timestamp now);

    // Non-null once the circuit breaker has disabled filtering; every event then passes unchanged.
    [[nodiscard]] bool tripped() const noexcept { return trip_reason_ != nullptr; }
    [[nodiscard]] const char* trip_reason() const noexcept { return trip_reason_; }

    [[nodiscard]] const ChatterFilter& filter() const noexcept { return filter_; }
    [[nodiscard]] ChatterFilter& filter() noexcept { return filter_; }

private:
    void reschedule();
    void watch_press(KeyCode code, bool dropped);
    void trip(const char* reason);
    void rearm();

    struct KeyWatch {
        std::uint16_t slow_drops = 0;
        bool has_last_drop = false;
        Timestamp last_drop{};
    };

    ChatterFilter filter_;
    IKeyboardOutput& output_;
    IWakeupScheduler& scheduler_;
    const IClock& clock_;
    std::optional<Timestamp> scheduled_;

    std::vector<KeyWatch> watch_;
    const char* trip_reason_ = nullptr;
};

}  // namespace kcf
