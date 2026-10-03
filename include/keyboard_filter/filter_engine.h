// SPDX-License-Identifier: MIT
#pragma once

#include <optional>

#include "keyboard_filter/chatter_filter.h"
#include "keyboard_filter/keyboard_io.h"

namespace kcf {

// Glue between an interceptor, the chatter filter, the output layer and the platform's timer.
// Identical on every platform; adapters only provide the four interfaces it talks to.
class FilterEngine final : public IKeyEventHandler {
public:
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

    void update_settings(const FilterSettings& settings);

    // Reconciles one key with its authoritative physical state (see ChatterFilter::resync_key).
    void resync_key(KeyCode code, bool physically_down, Timestamp now);

    [[nodiscard]] const ChatterFilter& filter() const noexcept { return filter_; }
    [[nodiscard]] ChatterFilter& filter() noexcept { return filter_; }

private:
    void reschedule();

    ChatterFilter filter_;
    IKeyboardOutput& output_;
    IWakeupScheduler& scheduler_;
    const IClock& clock_;
    std::optional<Timestamp> scheduled_;
};

}  // namespace kcf
