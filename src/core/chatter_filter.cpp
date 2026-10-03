// SPDX-License-Identifier: MIT
#include "keyboard_filter/chatter_filter.h"

#include <algorithm>

namespace kcf {

const char* to_string(KeyAction action) noexcept {
    switch (action) {
        case KeyAction::Down: return "down";
        case KeyAction::Up: return "up";
        case KeyAction::Repeat: return "repeat";
    }
    return "unknown";
}

const char* to_string(Decision decision) noexcept {
    switch (decision) {
        case Decision::Accept: return "accept";
        case Decision::Reject: return "reject";
        case Decision::Defer: return "defer";
    }
    return "unknown";
}

bool within_window(Timestamp earlier, Timestamp later, Duration window) noexcept {
    if (window <= Duration::zero()) {
        return false;
    }
    if (later >= earlier) {
        return later - earlier < window;
    }
    // The clock went backwards. A small step is jitter between timestamp sources: treat the two
    // events as simultaneous. A large step is a clock reset: the events are unrelated.
    return earlier - later <= window;
}

ChatterFilter::ChatterFilter(FilterSettings settings)
    : settings_(settings), keys_(kKeyCodeLimit) {}

Duration ChatterFilter::window_for(const KeyState& key) const noexcept {
    if (key.threshold_override >= Duration::zero()) {
        return key.threshold_override;
    }
    return settings_.threshold;
}

Duration ChatterFilter::threshold_for(KeyCode code) const noexcept {
    if (code >= kKeyCodeLimit) {
        return Duration::zero();
    }
    return window_for(keys_[code]);
}

void ChatterFilter::set_key_threshold(KeyCode code, std::optional<std::chrono::milliseconds> threshold) {
    if (code >= kKeyCodeLimit) {
        return;
    }
    keys_[code].threshold_override =
        threshold ? Duration(std::max(*threshold, std::chrono::milliseconds::zero())) : Duration(-1);
}

bool ChatterFilter::is_logically_down(KeyCode code) const noexcept {
    return code < kKeyCodeLimit && keys_[code].logical_down;
}

std::optional<Timestamp> ChatterFilter::next_deadline() const noexcept {
    if (!pending_) {
        return std::nullopt;
    }
    return keys_[*pending_].deadline;
}

std::optional<Timestamp> ChatterFilter::next_wakeup(Timestamp now) const noexcept {
    if (!pending_) {
        return std::nullopt;
    }
    const KeyState& key = keys_[*pending_];
    return std::max(now, std::min(key.deadline, now + window_for(key)));
}

void ChatterFilter::emit_pending(IEventSink& sink) {
    if (!pending_) {
        return;
    }
    KeyState& key = keys_[*pending_];
    pending_.reset();
    key.release_pending = false;
    key.logical_down = false;
    ++stats_.releases_emitted;
    sink.emit(key.pending_release);
}

void ChatterFilter::cancel_pending() noexcept {
    if (pending_) {
        keys_[*pending_].release_pending = false;
        pending_.reset();
    }
}

void ChatterFilter::flush_all(IEventSink& sink) {
    emit_pending(sink);
}

void ChatterFilter::flush_expired(Timestamp now, IEventSink& sink) {
    if (!pending_) {
        return;
    }
    const KeyState& key = keys_[*pending_];
    const Duration window = window_for(key);
    // A deadline lies at most one window past the release (plus one window of skew between the
    // event clock and the caller's clock). Anything further out comes from a mismatched clock.
    if (key.deadline <= now || key.deadline - now > 2 * window) {
        emit_pending(sink);
    }
}

void ChatterFilter::reset(IEventSink& sink) {
    flush_all(sink);
    for (KeyState& key : keys_) {
        const Duration override_value = key.threshold_override;
        key = KeyState{};
        key.threshold_override = override_value;
    }
}

void ChatterFilter::update_settings(const FilterSettings& settings, IEventSink& sink) {
    if (settings == settings_) {
        return;
    }
    reset(sink);
    settings_ = settings;
}

Decision ChatterFilter::process(const KeyEvent& event, IEventSink& sink) {
    ++stats_.events;

    // A release held back for another key goes out first: whatever this event is, applications
    // must observe it afterwards, exactly as the keys were physically operated.
    if (pending_ && *pending_ != event.code) {
        emit_pending(sink);
    }

    if (!settings_.enabled || event.code >= kKeyCodeLimit) {
        return Decision::Accept;
    }

    KeyState& key = keys_[event.code];
    const Duration window = window_for(key);
    if (window <= Duration::zero()) {
        // Filtering disabled for this key. Keep the bookkeeping coherent and pass everything.
        emit_pending(sink);
        key.known = true;
        key.physical_down = key.logical_down = event.action != KeyAction::Up;
        key.swallow_release = false;
        return Decision::Accept;
    }

    switch (event.action) {
        case KeyAction::Down: return on_down(key, event, window, sink);
        case KeyAction::Up: return on_up(key, event, window);
        case KeyAction::Repeat: return on_repeat(key);
    }
    return Decision::Accept;
}

Decision ChatterFilter::on_down(KeyState& key, const KeyEvent& event, Duration window, IEventSink& sink) {
    const Timestamp now = event.timestamp;

    if (!key.known) {
        key.known = true;
        key.physical_down = true;
        key.logical_down = true;
        key.last_down = now;
        key.has_last_down = true;
        return Decision::Accept;
    }

    if (key.physical_down) {
        // A press while the switch is already down. Right after the original press it is a
        // duplicate transition; OS auto-repeat only starts after the (much longer) repeat delay.
        // Later ones are auto-repeat on platforms that don't flag repeats (Windows).
        if (key.has_last_down && within_window(key.last_down, now, window)) {
            ++stats_.duplicates_suppressed;
            return Decision::Reject;
        }
        if (key.logical_down) {
            return Decision::Accept;
        }
        ++stats_.repeats_suppressed;
        return Decision::Reject;
    }

    // A new physical press.
    const bool bounced = key.has_last_up && within_window(key.last_up, now, window);
    key.physical_down = true;
    key.last_down = now;
    key.has_last_down = true;

    if (key.release_pending) {
        if (bounced) {
            // Release + re-press inside the window: switch chatter. Drop both; the key stays held.
            cancel_pending();
            ++stats_.chatter_suppressed;
            return Decision::Reject;
        }
        // The window elapsed but the platform timer has not fired yet: deliver the release first.
        emit_pending(sink);
        key.logical_down = true;
        return Decision::Accept;
    }

    if (key.logical_down) {
        // Applications already consider the key held; another press would read as a repeat.
        return Decision::Accept;
    }

    if (bounced) {
        // The release was already delivered (another key intervened), so the bounce cannot be
        // merged into the original press anymore. Drop the spurious press and, later, its release.
        key.swallow_release = true;
        ++stats_.chatter_suppressed;
        return Decision::Reject;
    }

    key.logical_down = true;
    return Decision::Accept;
}

Decision ChatterFilter::on_up(KeyState& key, const KeyEvent& event, Duration window) {
    const Timestamp now = event.timestamp;

    if (!key.known) {
        // We never saw this key go down (e.g. it was held when filtering started). Let the release
        // through so nothing stays stuck downstream.
        key.known = true;
        key.physical_down = false;
        key.logical_down = false;
        key.last_up = now;
        key.has_last_up = true;
        return Decision::Accept;
    }

    if (!key.physical_down) {
        // Release without a press in between.
        if (key.release_pending || (key.has_last_up && within_window(key.last_up, now, window))) {
            ++stats_.duplicates_suppressed;
            return Decision::Reject;
        }
        // A stray, stale release is harmless and can only help resynchronise applications.
        return Decision::Accept;
    }

    key.physical_down = false;
    key.last_up = now;
    key.has_last_up = true;

    if (key.swallow_release) {
        key.swallow_release = false;
        return Decision::Reject;
    }

    if (!key.logical_down) {
        return Decision::Accept;
    }

    key.pending_release = event;
    key.deadline = now + window;
    key.release_pending = true;
    pending_ = event.code;
    ++stats_.releases_deferred;
    return Decision::Defer;
}

Decision ChatterFilter::on_repeat(KeyState& key) {
    if (!key.known) {
        // Held since before we started: applications saw the press, so let repeats continue.
        key.known = true;
        key.physical_down = true;
        key.logical_down = true;
        return Decision::Accept;
    }
    if (key.logical_down && !key.release_pending) {
        return Decision::Accept;
    }
    // Auto-repeat for a key applications consider released (its press was dropped as chatter).
    ++stats_.repeats_suppressed;
    return Decision::Reject;
}

void ChatterFilter::resync_key(KeyCode code, bool physically_down, Timestamp now, IEventSink& sink) {
    if (code >= kKeyCodeLimit) {
        return;
    }
    KeyState& key = keys_[code];
    key.known = true;
    key.swallow_release = false;

    KeyEvent synthesized{};
    synthesized.code = code;
    synthesized.timestamp = now;

    if (physically_down) {
        if (!key.physical_down) {
            key.physical_down = true;
            key.last_down = now;
            key.has_last_down = true;
        }
        if (key.release_pending) {
            cancel_pending();
        }
        if (!key.logical_down) {
            key.logical_down = true;
            synthesized.action = KeyAction::Down;
            sink.emit(synthesized);
        }
        return;
    }

    if (key.physical_down) {
        key.physical_down = false;
        key.last_up = now;
        key.has_last_up = true;
    }
    if (key.release_pending) {
        emit_pending(sink);
    } else if (key.logical_down) {
        key.logical_down = false;
        synthesized.action = KeyAction::Up;
        sink.emit(synthesized);
    }
}

}  // namespace kcf
