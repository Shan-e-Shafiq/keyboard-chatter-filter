// SPDX-License-Identifier: MIT
#include "keyboard_filter/filter_engine.h"

namespace kcf {

FilterEngine::FilterEngine(FilterSettings settings, IKeyboardOutput& output, IWakeupScheduler& scheduler,
                           const IClock& clock)
    : filter_(settings), output_(output), scheduler_(scheduler), clock_(clock) {}

Decision FilterEngine::on_key_event(const KeyEvent& event) {
    const Decision decision = filter_.process(event, output_);
    reschedule();
    return decision;
}

void FilterEngine::on_events_lost() {
    filter_.reset(output_);
    reschedule();
}

void FilterEngine::on_wakeup() {
    // The timer is one-shot: forget it so reschedule() re-arms it if work remains.
    scheduled_.reset();
    filter_.flush_expired(clock_.now(), output_);
    reschedule();
}

void FilterEngine::flush() {
    filter_.flush_all(output_);
    reschedule();
}

void FilterEngine::update_settings(const FilterSettings& settings) {
    filter_.update_settings(settings, output_);
    reschedule();
}

void FilterEngine::resync_key(KeyCode code, bool physically_down, Timestamp now) {
    filter_.resync_key(code, physically_down, now, output_);
    reschedule();
}

void FilterEngine::reschedule() {
    const std::optional<Timestamp> wanted =
        filter_.has_pending() ? filter_.next_wakeup(clock_.now()) : std::nullopt;
    if (wanted == scheduled_) {
        return;
    }
    scheduled_ = wanted;
    scheduler_.schedule_wakeup(wanted);
}

}  // namespace kcf
