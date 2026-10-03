// SPDX-License-Identifier: MIT
//
// Behavioural tests for the platform-independent chatter filter, driven through the fake
// keyboard (tests/fake_keyboard.h) so every test sees exactly what applications would see.

#include <algorithm>
#include <map>
#include <random>
#include <tuple>

#include "fake_keyboard.h"
#include "keyboard_filter/chatter_filter.h"
#include "test_framework.h"

using namespace kcf;
using namespace kcf::testing;
using namespace std::chrono_literals;

namespace {

constexpr KeyCode kA = 'a';
constexpr KeyCode kB = 'b';
constexpr KeyCode kC = 'c';
constexpr KeyCode kShift = 'S';
constexpr KeyCode kCtrl = 'C';

class RecordingSink final : public IEventSink {
public:
    void emit(const KeyEvent& event) override { events.push_back(event); }
    std::vector<KeyEvent> events;
};

KeyEvent ev(KeyCode code, KeyAction action, double ms) {
    return SimulatedKeyboard::make(code, action, ms);
}

int count(const SimulatedKeyboard& kb, KeyCode code, KeyAction action) {
    return static_cast<int>(std::count_if(kb.delivered().begin(), kb.delivered().end(), [&](const Delivered& d) {
        return d.event.code == code && d.event.action == action;
    }));
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Normal operation
// ---------------------------------------------------------------------------------------------

TEST(Basic, NormalPressIsDeliveredImmediately) {
    SimulatedKeyboard kb;
    EXPECT_EQ(kb.press(kA, 0), Decision::Accept);
    EXPECT_EQ(kb.transcript(), "a+");
    EXPECT_EQ(kb.delivered()[0].delivered_at, at_ms(0));
    EXPECT_FALSE(kb.delivered()[0].injected);
}

TEST(Basic, NormalReleaseIsDeliveredAfterTheChatterWindow) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    EXPECT_EQ(kb.release(kA, 80), Decision::Defer);
    EXPECT_EQ(kb.transcript(), "a+");
    kb.advance_to_ms(109.9);
    EXPECT_EQ(kb.transcript(), "a+");
    kb.advance_to_ms(110);
    EXPECT_EQ(kb.transcript(), "a+ a-");
    EXPECT_EQ(kb.delivered()[1].delivered_at, at_ms(110));
    EXPECT_TRUE(kb.delivered()[1].injected);
    EXPECT_FALSE(kb.filter().is_logically_down(kA));
}

TEST(Basic, AcceptedEventsRemainUnchanged) {
    SimulatedKeyboard kb;
    KeyEvent down = ev(kA, KeyAction::Down, 5);
    down.modifiers = ModifierState{}.set(Modifier::LeftShift).set(Modifier::CapsLock);
    down.device = 42;
    KeyEvent up = down;
    up.action = KeyAction::Up;
    up.timestamp = at_ms(90);

    kb.send(down);
    kb.send(up);
    kb.settle();

    ASSERT_EQ(kb.delivered().size(), 2u);
    EXPECT_TRUE(kb.delivered()[0].event == down);
    EXPECT_TRUE(kb.delivered()[1].event == up);  // the deferred release is the original event, untouched
}

TEST(Basic, DeferredReleaseIsScheduledAndCancelled) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    EXPECT_FALSE(kb.scheduler().wakeup.has_value());
    kb.release(kA, 100);
    ASSERT_TRUE(kb.scheduler().wakeup.has_value());
    EXPECT_EQ(*kb.scheduler().wakeup, at_ms(130));
    kb.press(kA, 104);  // bounce cancels the pending release
    EXPECT_FALSE(kb.scheduler().wakeup.has_value());
}

TEST(Basic, StatsCountDeferredAndEmittedReleases) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 80);
    kb.settle();
    EXPECT_EQ(kb.filter().stats().events, 2u);
    EXPECT_EQ(kb.filter().stats().releases_deferred, 1u);
    EXPECT_EQ(kb.filter().stats().releases_emitted, 1u);
    EXPECT_EQ(kb.filter().stats().chatter_suppressed, 0u);
}

// ---------------------------------------------------------------------------------------------
// Chatter: release followed by a re-press of the same key inside the window
// ---------------------------------------------------------------------------------------------

namespace {
void expect_release_chatter_suppressed(double gap_ms) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 100);
    EXPECT_EQ(kb.press(kA, 100 + gap_ms), Decision::Reject);
    EXPECT_EQ(kb.release(kA, 100 + gap_ms + 2), Decision::Defer);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a-");
    EXPECT_EQ(kb.filter().stats().chatter_suppressed, 1u);
    // The surviving release is delivered one window after the *last* physical release.
    EXPECT_EQ(kb.delivered().back().delivered_at, at_ms(100 + gap_ms + 2 + 30));
}
}  // namespace

TEST(Chatter, Within5ms) { expect_release_chatter_suppressed(5); }
TEST(Chatter, Within10ms) { expect_release_chatter_suppressed(10); }
TEST(Chatter, Within20ms) { expect_release_chatter_suppressed(20); }
TEST(Chatter, Within30ms) { expect_release_chatter_suppressed(29); }
TEST(Chatter, JustInside30ms) { expect_release_chatter_suppressed(29.999); }
TEST(Chatter, ZeroGap) { expect_release_chatter_suppressed(0); }

TEST(Chatter, BounceOnPressKeepsKeyHeld) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 1.5);
    kb.press(kA, 3);
    kb.release(kA, 4);
    kb.press(kA, 6);
    kb.advance_to_ms(80);
    EXPECT_EQ(kb.transcript(), "a+");
    EXPECT_TRUE(kb.filter().is_logically_down(kA));
    kb.release(kA, 150);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a-");
    EXPECT_EQ(kb.filter().stats().chatter_suppressed, 2u);
}

TEST(Chatter, BounceOnReleaseProducesSingleKeystroke) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 90);
    kb.press(kA, 92);
    kb.release(kA, 93);
    kb.press(kA, 96);
    kb.release(kA, 97);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a-");
    EXPECT_EQ(kb.delivered().back().delivered_at, at_ms(127));
}

TEST(Chatter, ContactLossWhileHeldDoesNotReleaseTheKey) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 400);
    kb.press(kA, 404);
    kb.advance_to_ms(600);
    EXPECT_EQ(kb.transcript(), "a+");
    EXPECT_TRUE(kb.filter().is_logically_down(kA));
    kb.release(kA, 700);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a-");
}

TEST(Chatter, RejectedEventsNeverReachTheOutputLayer) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 100);
    kb.press(kA, 103);   // rejected
    kb.release(kA, 105);
    kb.press(kA, 108);   // rejected
    kb.release(kA, 110);
    kb.settle();
    // The output layer only ever saw the single genuine release, never a chatter press.
    ASSERT_EQ(kb.injected().size(), 1u);
    EXPECT_EQ(kb.injected()[0].action, KeyAction::Up);
    EXPECT_EQ(kb.injected()[0].timestamp, at_ms(110));
    for (const KeyEvent& e : kb.injected()) {
        EXPECT_NE(e.action, KeyAction::Down);
    }
}

// ---------------------------------------------------------------------------------------------
// Threshold boundaries and configurability
// ---------------------------------------------------------------------------------------------

namespace {
std::string tap_twice(FilterSettings settings, double gap_ms) {
    SimulatedKeyboard kb(settings);
    kb.press(kA, 0);
    kb.release(kA, 60);
    kb.press(kA, 60 + gap_ms);
    kb.release(kA, 120 + gap_ms);
    kb.settle();
    return kb.transcript();
}
}  // namespace

TEST(Threshold, ExactlyAtThresholdIsLegitimate) {
    EXPECT_EQ(tap_twice({}, 30), "a+ a- a+ a-");
}

TEST(Threshold, JustOutsideThresholdIsLegitimate) {
    EXPECT_EQ(tap_twice({}, 30.001), "a+ a- a+ a-");
    EXPECT_EQ(tap_twice({}, 31), "a+ a- a+ a-");
    EXPECT_EQ(tap_twice({}, 45), "a+ a- a+ a-");
}

TEST(Threshold, JustInsideThresholdIsChatter) {
    EXPECT_EQ(tap_twice({}, 29.999), "a+ a-");
}

TEST(Threshold, LowerThresholdLetsFasterRepressesThrough) {
    const FilterSettings settings{10ms, true};
    EXPECT_EQ(tap_twice(settings, 15), "a+ a- a+ a-");
    EXPECT_EQ(tap_twice(settings, 9), "a+ a-");
}

TEST(Threshold, HigherThresholdCatchesSlowerChatter) {
    const FilterSettings settings{50ms, true};
    EXPECT_EQ(tap_twice(settings, 40), "a+ a-");
    EXPECT_EQ(tap_twice(settings, 50), "a+ a- a+ a-");
}

TEST(Threshold, ReleaseLatencyFollowsThreshold) {
    SimulatedKeyboard kb(FilterSettings{12ms, true});
    kb.press(kA, 0);
    kb.release(kA, 50);
    kb.settle();
    EXPECT_EQ(kb.delivered().back().delivered_at, at_ms(62));
}

TEST(Threshold, PerKeyOverride) {
    SimulatedKeyboard kb;
    kb.engine().filter().set_key_threshold(kA, 60ms);
    EXPECT_EQ(kb.filter().threshold_for(kA), Duration(60ms));
    EXPECT_EQ(kb.filter().threshold_for(kB), Duration(30ms));

    kb.press(kA, 0);
    kb.release(kA, 100);
    EXPECT_EQ(kb.press(kA, 145), Decision::Reject);  // 45 ms: chatter for 'a' (60 ms window)
    kb.release(kA, 200);
    kb.press(kB, 400);
    kb.release(kB, 500);
    EXPECT_EQ(kb.press(kB, 545), Decision::Accept);  // 45 ms: legitimate for 'b' (30 ms window)
    kb.release(kB, 600);
    kb.settle();
    EXPECT_EQ(count(kb, kA, KeyAction::Down), 1);
    EXPECT_EQ(count(kb, kB, KeyAction::Down), 2);

    kb.engine().filter().set_key_threshold(kA, std::nullopt);
    EXPECT_EQ(kb.filter().threshold_for(kA), Duration(30ms));
}

TEST(Threshold, PerKeyZeroDisablesFilteringForThatKey) {
    SimulatedKeyboard kb;
    kb.engine().filter().set_key_threshold(kC, 0ms);
    kb.press(kC, 0);
    EXPECT_EQ(kb.release(kC, 2), Decision::Accept);
    EXPECT_EQ(kb.press(kC, 3), Decision::Accept);
    EXPECT_EQ(kb.release(kC, 4), Decision::Accept);
    EXPECT_EQ(kb.transcript(), "c+ c- c+ c-");
}

// ---------------------------------------------------------------------------------------------
// Duplicate transitions
// ---------------------------------------------------------------------------------------------

TEST(Duplicates, DuplicateKeyDownIsRejected) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    EXPECT_EQ(kb.press(kA, 3), Decision::Reject);
    EXPECT_EQ(kb.press(kA, 25), Decision::Reject);
    kb.release(kA, 80);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a-");
    EXPECT_EQ(kb.filter().stats().duplicates_suppressed, 2u);
}

TEST(Duplicates, DuplicateKeyUpIsRejected) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 80);
    EXPECT_EQ(kb.release(kA, 83), Decision::Reject);  // while the first release is held back
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a-");
}

TEST(Duplicates, DuplicateKeyUpAfterReleaseWasDeliveredIsRejected) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 80);
    kb.press(kB, 85);                                 // delivers a's release early
    EXPECT_EQ(kb.release(kA, 90), Decision::Reject);  // still inside a's window
    kb.release(kB, 140);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a- b+ b-");
}

TEST(Duplicates, StaleReleaseWithoutPressPassesThrough) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 80);
    kb.settle();
    // Long after: harmless, and helps resynchronise anything downstream.
    EXPECT_EQ(kb.release(kA, 5000), Decision::Accept);
}

TEST(Duplicates, ReleaseOfUnknownKeyPassesThrough) {
    // Key held before filtering started: its press was never seen.
    SimulatedKeyboard kb;
    EXPECT_EQ(kb.release(kA, 10), Decision::Accept);
    EXPECT_EQ(kb.transcript(), "a-");
    EXPECT_FALSE(kb.filter().is_logically_down(kA));
}

// ---------------------------------------------------------------------------------------------
// Rapid legitimate typing
// ---------------------------------------------------------------------------------------------

TEST(Typing, HumanSpeedDoubleLetter) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 70);
    kb.press(kA, 120);
    kb.release(kA, 190);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a- a+ a-");
}

TEST(Typing, IntentionalRapidTappingJustAboveThreshold) {
    SimulatedKeyboard kb;
    double t = 0;
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(kb.press(kA, t), Decision::Accept);
        kb.release(kA, t + 35);
        t += 35 + 31;  // 31 ms between release and the next press
    }
    kb.settle();
    EXPECT_EQ(count(kb, kA, KeyAction::Down), 20);
    EXPECT_EQ(count(kb, kA, KeyAction::Up), 20);
}

TEST(Typing, RolloverKeepsInputOrder) {
    // "the" typed with overlapping keys.
    SimulatedKeyboard kb;
    kb.press('t', 0);
    kb.press('h', 60);
    kb.release('t', 75);
    kb.press('e', 110);
    kb.release('h', 130);
    kb.release('e', 190);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "t+ h+ t- e+ h- e-");
}

TEST(Typing, NextKeyFlushesHeldBackRelease) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 50);
    kb.press(kB, 55);
    EXPECT_EQ(kb.transcript(), "a+ a- b+");
    EXPECT_EQ(kb.delivered()[1].delivered_at, at_ms(55));
}

TEST(Typing, AlternatingKeysAtHumanSpeed) {
    SimulatedKeyboard kb;
    double t = 0;
    for (int i = 0; i < 10; ++i) {
        const KeyCode key = i % 2 == 0 ? kA : kB;
        kb.press(key, t);
        kb.release(key, t + 50);
        t += 65;
    }
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a- b+ b- a+ a- b+ b- a+ a- b+ b- a+ a- b+ b- a+ a- b+ b-");
}

// ---------------------------------------------------------------------------------------------
// Held keys and auto-repeat
// ---------------------------------------------------------------------------------------------

TEST(Repeat, FlaggedRepeatsPassWhileHeld) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    EXPECT_EQ(kb.repeat(kA, 500), Decision::Accept);
    EXPECT_EQ(kb.repeat(kA, 533), Decision::Accept);
    EXPECT_EQ(kb.repeat(kA, 566), Decision::Accept);
    kb.release(kA, 590);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a* a* a* a-");
}

TEST(Repeat, FastRepeatRatePasses) {
    // Some users configure 10 ms repeat intervals; repeats are never mistaken for chatter.
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(kb.repeat(kA, 200 + i * 10), Decision::Accept);
    }
    kb.release(kA, 800);
    kb.settle();
    EXPECT_EQ(count(kb, kA, KeyAction::Repeat), 50);
}

TEST(Repeat, UnflaggedRepeatsPassWhileHeld) {
    // Windows low-level hooks report auto-repeat as further key-downs without a flag.
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    EXPECT_EQ(kb.press(kA, 250), Decision::Accept);
    EXPECT_EQ(kb.press(kA, 283), Decision::Accept);
    EXPECT_EQ(kb.press(kA, 316), Decision::Accept);
    kb.release(kA, 330);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a+ a+ a+ a-");
}

TEST(Repeat, RepeatContinuesAfterChatterWhileHeld) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.repeat(kA, 500);
    kb.release(kA, 520);  // contact loss
    kb.press(kA, 523);
    EXPECT_EQ(kb.repeat(kA, 1000), Decision::Accept);
    kb.release(kA, 1100);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a* a* a-");
}

TEST(Repeat, RepeatOfKeyWhosePressWasDroppedIsRejected) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 100);
    kb.press(kB, 102);                               // forces a's release out
    EXPECT_EQ(kb.press(kA, 105), Decision::Reject);  // bounce after the release was delivered
    kb.release(kB, 150);
    EXPECT_EQ(kb.repeat(kA, 600), Decision::Reject);
    EXPECT_EQ(kb.press(kA, 640), Decision::Reject);  // unflagged repeat, same situation
    EXPECT_EQ(kb.release(kA, 700), Decision::Reject);
    kb.settle();
    EXPECT_EQ(count(kb, kA, KeyAction::Down), 1);
    EXPECT_EQ(count(kb, kA, KeyAction::Up), 1);
    EXPECT_EQ(count(kb, kA, KeyAction::Repeat), 0);
}

TEST(Repeat, RepeatsOfAnotherKeyDoNotBreakChatterDetection) {
    SimulatedKeyboard kb;
    kb.press('j', 0);
    kb.repeat('j', 300);
    kb.press(kA, 310);
    kb.repeat('j', 333);
    kb.release(kA, 360);
    kb.repeat('j', 366);  // forces a's release out before the bounce
    kb.press(kA, 368);    // bounce
    kb.release(kA, 370);
    kb.repeat('j', 399);
    kb.release('j', 420);
    kb.settle();
    EXPECT_EQ(count(kb, kA, KeyAction::Down), 1);
    EXPECT_EQ(count(kb, kA, KeyAction::Up), 1);
    EXPECT_EQ(count(kb, 'j', KeyAction::Repeat), 4);
}

TEST(Repeat, RepeatWithoutSeenPressPasses) {
    SimulatedKeyboard kb;
    EXPECT_EQ(kb.repeat(kA, 0), Decision::Accept);  // key held since before filtering started
    EXPECT_EQ(kb.repeat(kA, 33), Decision::Accept);
    kb.release(kA, 50);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a* a* a-");
}

// ---------------------------------------------------------------------------------------------
// Different keys close together / several keys at once
// ---------------------------------------------------------------------------------------------

TEST(MultiKey, DifferentKeysPressedCloseTogetherAreIndependent) {
    SimulatedKeyboard kb;
    EXPECT_EQ(kb.press(kA, 0), Decision::Accept);
    EXPECT_EQ(kb.press(kB, 1), Decision::Accept);
    EXPECT_EQ(kb.press(kC, 2), Decision::Accept);
    kb.release(kA, 60);
    kb.release(kB, 61);
    kb.release(kC, 62);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ b+ c+ a- b- c-");
}

TEST(MultiKey, OtherKeyBetweenReleaseAndPressIsNotChatter) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 60);
    kb.press(kB, 65);
    kb.release(kB, 120);
    kb.press(kA, 130);  // 70 ms after a's release
    kb.release(kA, 190);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a- b+ b- a+ a-");
}

TEST(MultiKey, ChatterOnOneKeyWhileOthersAreHeld) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.press(kB, 10);
    kb.release(kB, 200);
    kb.press(kB, 204);  // b bounces while a is held
    kb.release(kA, 300);
    kb.release(kB, 400);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ b+ a- b-");
}

TEST(MultiKey, SimultaneousChatterOnTwoKeys) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.press(kB, 5);
    kb.release(kA, 100);
    kb.release(kB, 101);
    kb.press(kA, 103);
    kb.press(kB, 104);
    kb.release(kA, 106);
    kb.release(kB, 107);
    kb.settle();
    EXPECT_EQ(count(kb, kA, KeyAction::Down), 1);
    EXPECT_EQ(count(kb, kB, KeyAction::Down), 1);
    EXPECT_EQ(count(kb, kA, KeyAction::Up), 1);
    EXPECT_EQ(count(kb, kB, KeyAction::Up), 1);
    EXPECT_FALSE(kb.filter().is_logically_down(kA));
    EXPECT_FALSE(kb.filter().is_logically_down(kB));
}

TEST(MultiKey, ChordOfManyKeys) {
    SimulatedKeyboard kb;
    const std::string keys = "qwertyuiop";
    for (std::size_t i = 0; i < keys.size(); ++i) {
        EXPECT_EQ(kb.press(static_cast<KeyCode>(keys[i]), static_cast<double>(i)), Decision::Accept);
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        kb.release(static_cast<KeyCode>(keys[i]), 200.0 + static_cast<double>(i));
    }
    kb.settle();
    EXPECT_EQ(kb.transcript(), "q+ w+ e+ r+ t+ y+ u+ i+ o+ p+ q- w- e- r- t- y- u- i- o- p-");
}

TEST(MultiKey, SameInstantReleasesOfManyKeysKeepOrder) {
    // Releases never pile up: each event of another key delivers the held-back one first.
    RecordingSink sink;
    ChatterFilter filter;
    const int keys = 40;
    for (int i = 0; i < keys; ++i) {
        EXPECT_EQ(filter.process(ev(static_cast<KeyCode>(300 + i), KeyAction::Down, 0), sink), Decision::Accept);
    }
    for (int i = 0; i < keys; ++i) {
        EXPECT_EQ(filter.process(ev(static_cast<KeyCode>(300 + i), KeyAction::Up, 100), sink), Decision::Defer);
        EXPECT_EQ(sink.events.size(), static_cast<std::size_t>(i));
    }
    filter.flush_all(sink);
    ASSERT_EQ(sink.events.size(), static_cast<std::size_t>(keys));
    for (int i = 0; i < keys; ++i) {
        EXPECT_EQ(sink.events[static_cast<std::size_t>(i)].code, static_cast<KeyCode>(300 + i));
    }
    EXPECT_FALSE(filter.has_pending());
}

// ---------------------------------------------------------------------------------------------
// Modifier keys
// ---------------------------------------------------------------------------------------------

TEST(Modifiers, ShiftedLetter) {
    SimulatedKeyboard kb;
    kb.press(kShift, 0, KeyRole::Modifier);
    kb.press(kA, 50);
    kb.release(kA, 120);
    kb.release(kShift, 150, KeyRole::Modifier);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "S+ a+ a- S-");
}

TEST(Modifiers, ShiftReleaseReachesApplicationsBeforeTheNextLetter) {
    SimulatedKeyboard kb;
    kb.press(kShift, 0, KeyRole::Modifier);
    kb.press(kA, 40);
    kb.release(kA, 100);
    kb.release(kShift, 110, KeyRole::Modifier);
    kb.press(kB, 115);  // typed 5 ms after releasing Shift: must come out lowercase
    kb.release(kB, 170);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "S+ a+ a- S- b+ b-");
}

TEST(Modifiers, ShiftChatterWhileHeldKeepsShiftDown) {
    SimulatedKeyboard kb;
    kb.press(kShift, 0, KeyRole::Modifier);
    kb.release(kShift, 200, KeyRole::Modifier);
    kb.press(kShift, 203, KeyRole::Modifier);
    kb.press(kA, 250);
    EXPECT_TRUE(kb.filter().is_logically_down(kShift));
    kb.release(kA, 300);
    kb.release(kShift, 400, KeyRole::Modifier);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "S+ a+ a- S-");
}

TEST(Modifiers, ModifierPressBounce) {
    SimulatedKeyboard kb;
    kb.press(kShift, 0, KeyRole::Modifier);
    kb.release(kShift, 2, KeyRole::Modifier);
    kb.press(kShift, 5, KeyRole::Modifier);
    kb.press(kA, 40);
    kb.release(kA, 90);
    kb.release(kShift, 120, KeyRole::Modifier);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "S+ a+ a- S-");
}

TEST(Modifiers, RepeatedShortcutWithHeldModifier) {
    SimulatedKeyboard kb;
    kb.press(kCtrl, 0, KeyRole::Modifier);
    kb.press(kC, 40);
    kb.release(kC, 80);
    kb.press(kC, 130);
    kb.release(kC, 170);
    kb.release(kCtrl, 200, KeyRole::Modifier);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "C+ c+ c- c+ c- C-");
}

TEST(Modifiers, IntentionalDoubleTapOfModifier) {
    // e.g. double-tap Shift / Control shortcuts: ~100 ms between taps.
    SimulatedKeyboard kb;
    kb.press(kShift, 0, KeyRole::Modifier);
    kb.release(kShift, 60, KeyRole::Modifier);
    kb.press(kShift, 150, KeyRole::Modifier);
    kb.release(kShift, 210, KeyRole::Modifier);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "S+ S- S+ S-");
}

TEST(Modifiers, ModifierStateSnapshotIsPreserved) {
    SimulatedKeyboard kb;
    KeyEvent shift = ev(kShift, KeyAction::Down, 0);
    shift.role = KeyRole::Modifier;
    shift.modifiers = ModifierState{}.set(Modifier::LeftShift);
    KeyEvent letter = ev(kA, KeyAction::Down, 30);
    letter.modifiers = ModifierState{}.set(Modifier::LeftShift).set(Modifier::LeftAlt);
    kb.send(shift);
    kb.send(letter);
    ASSERT_EQ(kb.delivered().size(), 2u);
    EXPECT_EQ(kb.delivered()[0].event.modifiers.bits(), shift.modifiers.bits());
    EXPECT_EQ(kb.delivered()[1].event.modifiers.bits(), letter.modifiers.bits());
    EXPECT_TRUE(kb.delivered()[0].event.is_modifier());
}

// ---------------------------------------------------------------------------------------------
// Unusual but possible timestamp sequences
// ---------------------------------------------------------------------------------------------

TEST(Timestamps, IdenticalTimestamps) {
    SimulatedKeyboard kb;
    kb.press(kA, 100);
    kb.release(kA, 100);
    EXPECT_EQ(kb.press(kA, 100), Decision::Reject);
    kb.release(kA, 100);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a-");
}

TEST(Timestamps, SmallBackwardsStepIsTreatedAsSimultaneous) {
    RecordingSink sink;
    ChatterFilter filter;
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Down, 100), sink), Decision::Accept);
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Up, 150), sink), Decision::Defer);
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Down, 148), sink), Decision::Reject);
    EXPECT_TRUE(sink.events.empty());
}

TEST(Timestamps, LargeBackwardsJumpIsNotChatter) {
    // A clock reset must never cause a genuine keystroke to be dropped.
    RecordingSink sink;
    ChatterFilter filter;
    static_cast<void>(filter.process(ev(kA, KeyAction::Down, 1'000'000), sink));
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Up, 1'000'100), sink), Decision::Defer);
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Down, 50), sink), Decision::Accept);
    ASSERT_EQ(sink.events.size(), 1u);  // the held-back release was delivered first
    EXPECT_EQ(sink.events[0].action, KeyAction::Up);
}

TEST(Timestamps, LongGapLikeSystemSleep) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 50);
    kb.press(kA, 3'600'000);
    kb.release(kA, 3'600'060);
    kb.settle();
    EXPECT_EQ(kb.transcript(), "a+ a- a+ a-");
}

TEST(Timestamps, OutOfOrderAcrossDifferentKeys) {
    SimulatedKeyboard kb;
    RecordingSink sink;
    auto& filter = kb.engine().filter();
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Down, 100), sink), Decision::Accept);
    EXPECT_EQ(filter.process(ev(kB, KeyAction::Down, 90), sink), Decision::Accept);
    EXPECT_EQ(filter.process(ev(kB, KeyAction::Up, 150), sink), Decision::Defer);
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Up, 140), sink), Decision::Defer);
    ASSERT_EQ(sink.events.size(), 1u);  // b's release went out before a's event
    EXPECT_EQ(sink.events[0].code, kB);
}

TEST(Timestamps, TimerFiringLateStillDeliversReleaseFirst) {
    // The platform timer has not run yet when the next press arrives after the window.
    RecordingSink sink;
    ChatterFilter filter;
    static_cast<void>(filter.process(ev(kA, KeyAction::Down, 0), sink));
    static_cast<void>(filter.process(ev(kA, KeyAction::Up, 100), sink));
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Down, 140), sink), Decision::Accept);
    ASSERT_EQ(sink.events.size(), 1u);
    EXPECT_EQ(sink.events[0].action, KeyAction::Up);
}

TEST(Timestamps, MismatchedClockDomainCannotStickARelease) {
    // Event timestamps from a clock far ahead of the platform clock used for the timer.
    RecordingSink sink;
    ChatterFilter filter;
    const double far = 1e12;  // ~31 years ahead, in ms
    static_cast<void>(filter.process(ev(kA, KeyAction::Down, far), sink));
    EXPECT_EQ(filter.process(ev(kA, KeyAction::Up, far + 80), sink), Decision::Defer);

    const Timestamp now = at_ms(5000);
    const auto wake = filter.next_wakeup(now);
    ASSERT_TRUE(wake.has_value());
    EXPECT_EQ(*wake, now + Duration(30ms));
    filter.flush_expired(*wake, sink);
    ASSERT_EQ(sink.events.size(), 1u);
    EXPECT_FALSE(filter.has_pending());
}

TEST(Timestamps, SmallClockSkewDoesNotFlushEarly) {
    RecordingSink sink;
    ChatterFilter filter;
    static_cast<void>(filter.process(ev(kA, KeyAction::Down, 0), sink));
    static_cast<void>(filter.process(ev(kA, KeyAction::Up, 100), sink));
    filter.flush_expired(at_ms(95), sink);  // caller clock slightly behind the event clock
    EXPECT_TRUE(sink.events.empty());
    filter.flush_expired(at_ms(130), sink);
    EXPECT_EQ(sink.events.size(), 1u);
}

TEST(Timestamps, WithinWindowHelper) {
    EXPECT_TRUE(within_window(at_ms(0), at_ms(29), Duration(30ms)));
    EXPECT_FALSE(within_window(at_ms(0), at_ms(30), Duration(30ms)));
    EXPECT_TRUE(within_window(at_ms(10), at_ms(5), Duration(30ms)));
    EXPECT_FALSE(within_window(at_ms(100), at_ms(5), Duration(30ms)));
    EXPECT_FALSE(within_window(at_ms(0), at_ms(0), Duration::zero()));
}

// ---------------------------------------------------------------------------------------------
// Settings, resets and resynchronisation
// ---------------------------------------------------------------------------------------------

TEST(Control, DisabledFilterPassesEverything) {
    SimulatedKeyboard kb(FilterSettings{30ms, false});
    EXPECT_EQ(kb.press(kA, 0), Decision::Accept);
    EXPECT_EQ(kb.release(kA, 50), Decision::Accept);
    EXPECT_EQ(kb.press(kA, 52), Decision::Accept);
    EXPECT_EQ(kb.release(kA, 53), Decision::Accept);
    EXPECT_EQ(kb.transcript(), "a+ a- a+ a-");
}

TEST(Control, UpdatingSettingsFlushesHeldBackReleases) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 50);
    kb.engine().update_settings(FilterSettings{15ms, true});
    EXPECT_EQ(kb.transcript(), "a+ a-");
    EXPECT_EQ(kb.filter().settings().threshold, 15ms);
    kb.press(kA, 70);  // 20 ms later: legitimate under the new 15 ms threshold
    EXPECT_EQ(kb.transcript(), "a+ a- a+");
}

TEST(Control, DisablingAtRuntimeFlushesAndPasses) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 50);
    kb.engine().update_settings(FilterSettings{30ms, false});
    EXPECT_EQ(kb.transcript(), "a+ a-");
    EXPECT_EQ(kb.press(kA, 52), Decision::Accept);
}

TEST(Control, EventsLostResetsStateWithoutStickingKeys) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.press(kB, 10);
    kb.release(kA, 50);
    kb.engine().on_events_lost();
    EXPECT_EQ(kb.transcript(), "a+ b+ a-");
    EXPECT_FALSE(kb.filter().has_pending());
    // b's state was forgotten: its release passes straight through.
    EXPECT_EQ(kb.release(kB, 60), Decision::Accept);
}

TEST(Control, ResyncReleasesKeyThatIsPhysicallyUp) {
    RecordingSink sink;
    ChatterFilter filter;
    static_cast<void>(filter.process(ev(kA, KeyAction::Down, 0), sink));
    filter.resync_key(kA, false, at_ms(500), sink);
    ASSERT_EQ(sink.events.size(), 1u);
    EXPECT_EQ(sink.events[0].action, KeyAction::Up);
    EXPECT_FALSE(filter.is_logically_down(kA));
}

TEST(Control, ResyncDeliversHeldBackReleaseImmediately) {
    RecordingSink sink;
    ChatterFilter filter;
    static_cast<void>(filter.process(ev(kA, KeyAction::Down, 0), sink));
    static_cast<void>(filter.process(ev(kA, KeyAction::Up, 100), sink));
    filter.resync_key(kA, false, at_ms(105), sink);
    ASSERT_EQ(sink.events.size(), 1u);
    EXPECT_EQ(sink.events[0].timestamp, at_ms(100));  // the original release event
    EXPECT_FALSE(filter.has_pending());
}

TEST(Control, ResyncPressesKeyThatIsPhysicallyDown) {
    RecordingSink sink;
    ChatterFilter filter;
    filter.resync_key(kA, true, at_ms(10), sink);
    ASSERT_EQ(sink.events.size(), 1u);
    EXPECT_EQ(sink.events[0].action, KeyAction::Down);
    EXPECT_TRUE(filter.is_logically_down(kA));
    filter.resync_key(kA, true, at_ms(20), sink);  // already consistent: nothing to do
    EXPECT_EQ(sink.events.size(), 1u);
}

TEST(Control, ResyncCancelsHeldBackReleaseOfHeldKey) {
    RecordingSink sink;
    ChatterFilter filter;
    static_cast<void>(filter.process(ev(kA, KeyAction::Down, 0), sink));
    static_cast<void>(filter.process(ev(kA, KeyAction::Up, 100), sink));
    filter.resync_key(kA, true, at_ms(105), sink);  // it is actually down: the release was chatter
    EXPECT_TRUE(sink.events.empty());
    EXPECT_TRUE(filter.is_logically_down(kA));
    EXPECT_FALSE(filter.has_pending());
}

TEST(Control, OutOfRangeKeyCodesPassAndKeepOrder) {
    SimulatedKeyboard kb;
    kb.press(kA, 0);
    kb.release(kA, 50);
    const KeyCode big = static_cast<KeyCode>(kKeyCodeLimit + 5);
    EXPECT_EQ(kb.press(big, 52), Decision::Accept);
    EXPECT_EQ(kb.release(big, 53), Decision::Accept);
    EXPECT_EQ(kb.press(big, 54), Decision::Accept);  // never filtered
    EXPECT_EQ(kb.transcript(), "a+ a- #2053+ #2053- #2053+");
}

// ---------------------------------------------------------------------------------------------
// Randomised property tests
// ---------------------------------------------------------------------------------------------

namespace {

struct Generated {
    std::vector<KeyEvent> events;
    std::map<KeyCode, int> intended_presses;
};

// Generates interleaved human-like typing on several keys. Every intended press is held for
// 40-250 ms and the same key is not pressed again for at least `min_gap` ms. With `chatter`, each
// press/release may be followed by a short bounce train (gaps of 0-8 ms).
Generated generate(std::mt19937& rng, int keys, int presses_per_key, double min_gap, bool chatter) {
    Generated g;
    std::uniform_real_distribution<double> hold(40, 250);
    std::uniform_real_distribution<double> gap(min_gap, min_gap + 400);
    std::uniform_real_distribution<double> bounce(0.2, 8);
    std::uniform_int_distribution<int> bounces(0, 3);
    std::uniform_real_distribution<double> start(0, 300);
    std::bernoulli_distribution chatter_here(0.4);

    for (int k = 0; k < keys; ++k) {
        const KeyCode code = static_cast<KeyCode>(100 + k);
        double t = start(rng);
        for (int p = 0; p < presses_per_key; ++p) {
            const double down = t;
            const double up = down + hold(rng);
            g.events.push_back(ev(code, KeyAction::Down, down));
            double last = down;
            if (chatter && chatter_here(rng)) {  // bounce right after the press
                for (int b = bounces(rng); b > 0; --b) {
                    const double u = last + bounce(rng);
                    const double d = u + bounce(rng);
                    if (d >= up - 1) {
                        break;
                    }
                    g.events.push_back(ev(code, KeyAction::Up, u));
                    g.events.push_back(ev(code, KeyAction::Down, d));
                    last = d;
                }
            }
            g.events.push_back(ev(code, KeyAction::Up, up));
            last = up;
            if (chatter && chatter_here(rng)) {  // bounce right after the release
                for (int b = bounces(rng); b > 0; --b) {
                    const double d = last + bounce(rng);
                    const double u = d + bounce(rng);
                    g.events.push_back(ev(code, KeyAction::Down, d));
                    g.events.push_back(ev(code, KeyAction::Up, u));
                    last = u;
                }
            }
            ++g.intended_presses[code];
            t = last + gap(rng);
        }
    }
    std::stable_sort(g.events.begin(), g.events.end(),
                     [](const KeyEvent& a, const KeyEvent& b) { return a.timestamp < b.timestamp; });
    return g;
}

using EventKey = std::tuple<KeyCode, KeyAction, std::int64_t>;
EventKey key_of(const KeyEvent& e) { return {e.code, e.action, e.timestamp.count()}; }

void check_invariants(const SimulatedKeyboard& kb, const Generated& g) {
    std::map<EventKey, std::size_t> input_index;
    for (std::size_t i = 0; i < g.events.size(); ++i) {
        input_index.emplace(key_of(g.events[i]), i);
    }

    std::map<KeyCode, bool> down;
    std::map<KeyCode, int> presses;
    std::size_t previous_index = 0;
    bool first = true;
    for (const Delivered& d : kb.delivered()) {
        // Every delivered event is an unmodified input event...
        const auto it = input_index.find(key_of(d.event));
        ASSERT_TRUE(it != input_index.end());
        // ...delivered in input order.
        if (!first) {
            EXPECT_LT(previous_index, it->second);
        }
        previous_index = it->second;
        first = false;
        // Per key, applications see strictly alternating press/release.
        bool& is_down = down[d.event.code];
        if (d.event.action == KeyAction::Down) {
            EXPECT_FALSE(is_down);
            is_down = true;
            ++presses[d.event.code];
        } else if (d.event.action == KeyAction::Up) {
            EXPECT_TRUE(is_down);
            is_down = false;
        }
    }
    // No key is left stuck, and every intended keystroke arrived exactly once.
    for (const auto& [code, is_down] : down) {
        EXPECT_FALSE(is_down);
        EXPECT_FALSE(kb.filter().is_logically_down(code));
    }
    for (const auto& [code, intended] : g.intended_presses) {
        EXPECT_EQ(presses[code], intended);
    }
}

}  // namespace

TEST(Property, ChatterIsRemovedWithoutLosingKeystrokes) {
    for (unsigned seed = 1; seed <= 200; ++seed) {
        std::mt19937 rng(seed);
        const Generated g = generate(rng, 6, 25, 31, true);
        SimulatedKeyboard kb;
        for (const KeyEvent& e : g.events) {
            kb.send(e);
        }
        kb.settle();
        check_invariants(kb, g);
        if (kcf_test::failure_count() != 0) {
            std::fprintf(stderr, "    failing seed: %u\n", seed);
            return;
        }
    }
}

TEST(Property, CleanInputPassesThroughUnchanged) {
    for (unsigned seed = 1; seed <= 200; ++seed) {
        std::mt19937 rng(seed);
        const Generated g = generate(rng, 8, 25, 31, false);
        SimulatedKeyboard kb;
        for (const KeyEvent& e : g.events) {
            kb.send(e);
        }
        kb.settle();
        // Identical events, identical order: the filter is transparent to non-chattering input.
        ASSERT_EQ(kb.delivered().size(), g.events.size());
        for (std::size_t i = 0; i < g.events.size(); ++i) {
            EXPECT_TRUE(kb.delivered()[i].event == g.events[i]);
        }
        EXPECT_EQ(kb.filter().stats().chatter_suppressed, 0u);
        if (kcf_test::failure_count() != 0) {
            std::fprintf(stderr, "    failing seed: %u\n", seed);
            return;
        }
    }
}

TEST(Property, ReleaseLatencyIsBoundedByThreshold) {
    std::mt19937 rng(7);
    const Generated g = generate(rng, 5, 30, 31, false);
    SimulatedKeyboard kb;
    for (const KeyEvent& e : g.events) {
        kb.send(e);
    }
    kb.settle();
    for (const Delivered& d : kb.delivered()) {
        const Duration latency = d.delivered_at - d.event.timestamp;
        EXPECT_GE(latency, Duration::zero());
        EXPECT_LE(latency, Duration(30ms));
    }
}

KCF_TEST_MAIN()
