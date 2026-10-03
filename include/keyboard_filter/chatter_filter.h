// SPDX-License-Identifier: MIT
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "keyboard_filter/key_event.h"

namespace kcf {

// What the caller must do with the event it just passed to ChatterFilter::process().
enum class Decision : std::uint8_t {
    Accept,  // deliver the event unchanged, right now
    Reject,  // drop the event; it was chatter (or a duplicate / orphaned repeat)
    Defer,   // drop the event for now; the filter owns it and will either emit it later through
             // the sink (it was a genuine release) or discard it (the key bounced back down)
};

[[nodiscard]] const char* to_string(Decision decision) noexcept;

// Receives events the filter releases outside of process()'s return value: deferred key-ups whose
// chatter window elapsed, and releases that must be delivered *before* the event currently being
// processed to keep the output in the same order as the input.
class IEventSink {
public:
    virtual void emit(const KeyEvent& event) = 0;

protected:
    ~IEventSink() = default;
};

struct FilterSettings {
    // A key that is released and pressed again within this window is treated as switch chatter.
    std::chrono::milliseconds threshold{30};
    bool enabled = true;

    friend bool operator==(const FilterSettings&, const FilterSettings&) = default;
};

struct FilterStats {
    std::uint64_t events = 0;                // hardware key events seen by process()
    std::uint64_t chatter_suppressed = 0;    // spurious re-presses removed (each one = one avoided duplicate keystroke)
    std::uint64_t duplicates_suppressed = 0; // repeated down-without-up / up-without-down transitions
    std::uint64_t repeats_suppressed = 0;    // auto-repeat for a key that is logically released
    std::uint64_t releases_deferred = 0;     // key-ups held back for the chatter window
    std::uint64_t releases_emitted = 0;      // held-back key-ups later delivered
};

// Per-key switch-chatter filter.
//
// Physical switch chatter shows up at the OS level as extra *transitions* of a single key: a
// release followed by a re-press of the same key a few milliseconds later (on press or on
// release of the switch, or as a brief contact loss while held). A human cannot lift a finger and
// press the same key again within a couple of tens of milliseconds, so the filter keys its
// decisions on the release-to-press gap of each individual key:
//
//   * Presses are delivered immediately (no added latency on key-down).
//   * Releases are held back for `threshold`. If the same key goes down again inside that window,
//     the release and the re-press were chatter: both are dropped and the key stays held, which
//     keeps long presses, modifier holds and OS auto-repeat intact.
//   * Held-back releases are delivered before any event of a *different* key, so the output stream
//     keeps the input order (Shift released before the next letter, chords stay chords).
//   * If a held-back release already had to be delivered and the key then bounces back down within
//     the window, the re-press and its release are dropped instead.
//   * A second press of a key that is already down within the window is a duplicate and dropped;
//     later ones are auto-repeat and pass while the key is logically held.
//
// The filter is single-threaded and allocation-free after construction. See
// docs/filter-algorithm.md for the full state machine.
class ChatterFilter {
public:
    explicit ChatterFilter(FilterSettings settings = {});

    // Classifies one hardware event. Before returning, the filter may emit earlier held-back
    // releases through `sink`; those must reach applications before the event itself.
    [[nodiscard]] Decision process(const KeyEvent& event, IEventSink& sink);

    // Emits every held-back release whose chatter window has elapsed at `now`. A deadline that lies
    // implausibly far in the future (clock-domain mismatch) is treated as elapsed so a release can
    // never get stuck.
    void flush_expired(Timestamp now, IEventSink& sink);

    // Emits every held-back release immediately (shutdown, interception restart, ordering across
    // independent filter instances).
    void flush_all(IEventSink& sink);

    // Emits held-back releases and forgets all per-key state. Use when events may have been missed
    // (event tap disabled, hook reinstalled, input buffer overflow).
    void reset(IEventSink& sink);

    // Reconciles one key with an authoritative physical state (e.g. after an evdev SYN_DROPPED),
    // emitting a synthesized press/release if what applications saw no longer matches.
    void resync_key(KeyCode code, bool physically_down, Timestamp now, IEventSink& sink);

    // Earliest time at which flush_expired() has work to do, if a release is held back.
    [[nodiscard]] std::optional<Timestamp> next_deadline() const noexcept;

    // When the caller should next call flush_expired(), given the current time. Equal to
    // next_deadline() in normal operation, but never more than one window after `now`, so a
    // timestamp from a mismatched clock cannot postpone a release indefinitely.
    [[nodiscard]] std::optional<Timestamp> next_wakeup(Timestamp now) const noexcept;
    [[nodiscard]] bool has_pending() const noexcept { return pending_.has_value(); }

    // Applies new settings. Held-back releases are flushed and per-key state is forgotten so the
    // new threshold never mixes with timings judged under the old one.
    void update_settings(const FilterSettings& settings, IEventSink& sink);
    [[nodiscard]] const FilterSettings& settings() const noexcept { return settings_; }

    // Per-key threshold override. std::nullopt restores the global threshold; zero disables
    // filtering for that key.
    void set_key_threshold(KeyCode code, std::optional<std::chrono::milliseconds> threshold);
    [[nodiscard]] Duration threshold_for(KeyCode code) const noexcept;

    [[nodiscard]] const FilterStats& stats() const noexcept { return stats_; }

    // Whether applications currently see `code` as held down (exposed for tests and diagnostics).
    [[nodiscard]] bool is_logically_down(KeyCode code) const noexcept;

private:
    struct KeyState {
        KeyEvent pending_release{};
        Timestamp last_down{};
        Timestamp last_up{};
        Timestamp deadline{};
        Duration threshold_override{-1};  // negative = use the global threshold
        bool known = false;               // any event seen since the last reset
        bool physical_down = false;       // state of the switch as last reported by the OS
        bool logical_down = false;        // state applications have been told about
        bool release_pending = false;     // a key-up is held back in pending_release
        bool swallow_release = false;     // a chatter press was dropped; drop its key-up too
        bool has_last_down = false;
        bool has_last_up = false;
    };

    Decision on_down(KeyState& key, const KeyEvent& event, Duration window, IEventSink& sink);
    Decision on_up(KeyState& key, const KeyEvent& event, Duration window);
    Decision on_repeat(KeyState& key);

    void emit_pending(IEventSink& sink);
    void cancel_pending() noexcept;
    [[nodiscard]] Duration window_for(const KeyState& key) const noexcept;
    void clear_key_state() noexcept;

    FilterSettings settings_;
    std::vector<KeyState> keys_;  // kKeyCodeLimit entries, allocated once
    // The key whose release is held back. There is never more than one: any event of another key
    // delivers it first, so releases cannot pile up.
    std::optional<KeyCode> pending_;
    FilterStats stats_{};
};

// True if `later` happened less than `window` after `earlier`. Timestamps that step backwards by
// no more than `window` are treated as simultaneous; larger backwards jumps mean the clock was
// reset and are treated as unrelated (not within the window), so clock anomalies never cause
// legitimate input to be dropped.
[[nodiscard]] bool within_window(Timestamp earlier, Timestamp later, Duration window) noexcept;

}  // namespace kcf
