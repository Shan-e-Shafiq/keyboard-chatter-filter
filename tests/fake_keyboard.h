// SPDX-License-Identifier: MIT
//
// Fake keyboard input/output layer for exercising the platform-independent filter end-to-end
// without hardware: a simulated interceptor feeds timestamped hardware events into FilterEngine,
// a recording output captures injected events, and a manual scheduler/clock stand in for the
// platform timer. The "application-visible" stream is what a real OS would deliver: accepted
// events in place, plus events injected through the output layer at the time they were emitted.
#pragma once

#include <algorithm>
#include <chrono>
#include <cmath>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "keyboard_filter/filter_engine.h"
#include "keyboard_filter/keyboard_io.h"

namespace kcf {
inline std::ostream& operator<<(std::ostream& os, Decision d) { return os << to_string(d); }
inline std::ostream& operator<<(std::ostream& os, KeyAction a) { return os << to_string(a); }
}  // namespace kcf

namespace kcf::testing {

inline Timestamp at_ms(double milliseconds) {
    return Timestamp(std::llround(milliseconds * 1'000'000.0));
}

class FakeClock final : public IClock {
public:
    [[nodiscard]] Timestamp now() const noexcept override { return now_; }
    void set(Timestamp t) { now_ = t; }

private:
    Timestamp now_{};
};

class ManualScheduler final : public IWakeupScheduler {
public:
    void schedule_wakeup(std::optional<Timestamp> at) override {
        wakeup = at;
        ++calls;
    }
    std::optional<Timestamp> wakeup;
    int calls = 0;
};

struct Delivered {
    KeyEvent event;
    Timestamp delivered_at{};
    bool injected = false;  // came through IKeyboardOutput (a deferred release) rather than passing through
};

class SimulatedKeyboard final : public IKeyboardOutput {
public:
    explicit SimulatedKeyboard(FilterSettings settings = {})
        : engine_(settings, *this, scheduler_, clock_) {}

    // IKeyboardOutput: the filter injects an event towards applications.
    void emit(const KeyEvent& event) override {
        delivered_.push_back({event, clock_.now(), true});
        injected_.push_back(event);
    }

    Decision send(const KeyEvent& event) {
        advance_to(event.timestamp);
        clock_.set(std::max(clock_.now(), event.timestamp));
        const Decision decision = engine_.on_key_event(event);
        decisions_.push_back(decision);
        if (decision == Decision::Accept) {
            delivered_.push_back({event, clock_.now(), false});
        }
        return decision;
    }

    // Delivers `event` when the platform clock reads `clock_ms`, whatever timestamp the event
    // carries (for simulating platforms that report bogus timestamps).
    Decision send_at_clock(const KeyEvent& event, double clock_ms) {
        advance_to(at_ms(clock_ms));
        const Decision decision = engine_.on_key_event(event);
        decisions_.push_back(decision);
        if (decision == Decision::Accept) {
            delivered_.push_back({event, clock_.now(), false});
        }
        return decision;
    }

    Decision press(KeyCode code, double ms, KeyRole role = KeyRole::Regular) {
        return send(make(code, KeyAction::Down, ms, role));
    }
    Decision release(KeyCode code, double ms, KeyRole role = KeyRole::Regular) {
        return send(make(code, KeyAction::Up, ms, role));
    }
    Decision repeat(KeyCode code, double ms, KeyRole role = KeyRole::Regular) {
        return send(make(code, KeyAction::Repeat, ms, role));
    }

    // Advances the clock, firing the platform timer at its scheduled times along the way.
    void advance_to(Timestamp t) {
        while (scheduler_.wakeup && *scheduler_.wakeup <= t) {
            clock_.set(std::max(clock_.now(), *scheduler_.wakeup));
            scheduler_.wakeup.reset();
            engine_.on_wakeup();
        }
        clock_.set(std::max(clock_.now(), t));
    }
    void advance_to_ms(double ms) { advance_to(at_ms(ms)); }

    // Lets every held-back release go out.
    void settle() { advance_to(clock_.now() + std::chrono::seconds(10)); }

    static KeyEvent make(KeyCode code, KeyAction action, double ms, KeyRole role = KeyRole::Regular) {
        KeyEvent event;
        event.code = code;
        event.action = action;
        event.role = role;
        event.timestamp = at_ms(ms);
        return event;
    }

    // Compact rendering of the application-visible stream: "a+" press, "a-" release, "a*" repeat.
    // Codes in the printable ASCII range render as that character, others as "#<code>".
    [[nodiscard]] std::string transcript() const {
        std::string out;
        for (const Delivered& d : delivered_) {
            if (!out.empty()) {
                out.push_back(' ');
            }
            const KeyCode code = d.event.code;
            if (code > 32 && code < 127) {
                out.push_back(static_cast<char>(code));
            } else {
                out += "#" + std::to_string(code);
            }
            out.push_back(d.event.action == KeyAction::Down ? '+' : d.event.action == KeyAction::Up ? '-' : '*');
        }
        return out;
    }

    [[nodiscard]] const std::vector<Delivered>& delivered() const { return delivered_; }
    [[nodiscard]] const std::vector<KeyEvent>& injected() const { return injected_; }
    [[nodiscard]] const std::vector<Decision>& decisions() const { return decisions_; }
    [[nodiscard]] FilterEngine& engine() { return engine_; }
    [[nodiscard]] const ChatterFilter& filter() const { return engine_.filter(); }
    [[nodiscard]] ManualScheduler& scheduler() { return scheduler_; }
    [[nodiscard]] FakeClock& clock() { return clock_; }

private:
    FakeClock clock_;
    ManualScheduler scheduler_;
    FilterEngine engine_;
    std::vector<Delivered> delivered_;
    std::vector<KeyEvent> injected_;
    std::vector<Decision> decisions_;
};

}  // namespace kcf::testing
