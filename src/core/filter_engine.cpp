// SPDX-License-Identifier: MIT
#include "keyboard_filter/filter_engine.h"

#include <algorithm>

namespace kcf {

FilterEngine::FilterEngine(FilterSettings settings, IKeyboardOutput& output, IWakeupScheduler& scheduler,
                           const IClock& clock)
    : filter_(settings), output_(output), scheduler_(scheduler), clock_(clock), watch_(kKeyCodeLimit) {}

Decision FilterEngine::on_key_event(const KeyEvent& event) {
    if (tripped()) {
        return Decision::Accept;
    }
    const Decision decision = filter_.process(event, output_);
    if (event.action == KeyAction::Down) {
        // Only presses that are dropped outright count: a bounce merged into a key that stays held
        // costs the user nothing, and a long flickering hold legitimately produces many of those.
        watch_press(event.code,
                    decision == Decision::Reject && filter_.last_reject_reason() != RejectReason::Chatter);
        if (tripped()) {
            // Our decisions cannot be trusted: deliver this press too.
            reschedule();
            return Decision::Accept;
        }
    }
    reschedule();
    return decision;
}

void FilterEngine::watch_press(KeyCode code, bool dropped) {
    if (code >= watch_.size()) {
        return;
    }
    KeyWatch& w = watch_[code];
    if (!dropped) {
        w = KeyWatch{};
        return;
    }
    // Measured with the platform clock at processing time, deliberately not with event timestamps,
    // which may be exactly what is broken.
    const Timestamp now = clock_.now();
    const Duration slow_gap = std::max<Duration>(kMinSlowDropGap, 2 * filter_.settings().threshold);
    if (!w.has_last_drop || now - w.last_drop >= slow_gap) {
        ++w.slow_drops;
    }
    w.has_last_drop = true;
    w.last_drop = now;
    if (w.slow_drops >= kRunawaySlowDrops) {
        trip("a key's deliberate presses kept being dropped; real chatter never does that");
    }
}

void FilterEngine::trip(const char* reason) {
    trip_reason_ = reason;
    // Release whatever is held back and forget all state; from now on everything passes.
    filter_.reset(output_);
}

void FilterEngine::rearm() {
    trip_reason_ = nullptr;
    for (KeyWatch& w : watch_) {
        w = KeyWatch{};
    }
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
    rearm();
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
