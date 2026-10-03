// SPDX-License-Identifier: MIT
//
// End-to-end test of the macOS adapter through the real Quartz event system:
//
//   synthetic HID-state key events -> our HID-level event tap -> filter -> session-level recorder tap
//
// Needs Accessibility permission for the process running the test (the terminal when run by hand),
// which hosted CI cannot grant; without it the test exits with 77 (reported as skipped by CTest).
// It types only F18/F19, which do nothing in normal applications.

#include <ApplicationServices/ApplicationServices.h>

#include <vector>

#include "keyboard_filter/filter_engine.h"
#include "macos/keyboard_interceptor.h"
#include "macos/keyboard_output.h"
#include "macos/mach_clock.h"
#include "test_framework.h"

using namespace kcf;

namespace {

constexpr CGKeyCode kF18 = 0x4F;
constexpr CGKeyCode kF19 = 0x50;

struct Record {
    CGKeyCode code;
    bool down;
    friend bool operator==(const Record&, const Record&) = default;
};

std::ostream& operator<<(std::ostream& os, const std::vector<Record>& records) {
    for (const auto& r : records) {
        os << (r.code == kF19 ? "F19" : "F18") << (r.down ? "+ " : "- ");
    }
    return os;
}

// Records what applications receive, from a listen-only tap after ours.
class Recorder {
public:
    Recorder() {
        const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) | CGEventMaskBit(kCGEventKeyUp);
        tap_ = CGEventTapCreate(kCGSessionEventTap, kCGTailAppendEventTap, kCGEventTapOptionListenOnly, mask,
                                &Recorder::callback, this);
        if (tap_ != nullptr) {
            source_ = CFMachPortCreateRunLoopSource(kCFAllocatorDefault, tap_, 0);
            CFRunLoopAddSource(CFRunLoopGetCurrent(), source_, kCFRunLoopCommonModes);
        }
    }
    ~Recorder() {
        if (source_ != nullptr) {
            CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source_, kCFRunLoopCommonModes);
            CFRelease(source_);
        }
        if (tap_ != nullptr) {
            CFMachPortInvalidate(tap_);
            CFRelease(tap_);
        }
    }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    [[nodiscard]] bool ok() const { return tap_ != nullptr; }
    std::vector<Record> take() { return std::exchange(records_, {}); }

private:
    static CGEventRef callback(CGEventTapProxy, CGEventType type, CGEventRef event, void* info) {
        auto* self = static_cast<Recorder*>(info);
        const auto code = static_cast<CGKeyCode>(CGEventGetIntegerValueField(event, kCGKeyboardEventKeycode));
        if ((type == kCGEventKeyDown || type == kCGEventKeyUp) && (code == kF18 || code == kF19) &&
            CGEventGetIntegerValueField(event, kCGKeyboardEventAutorepeat) == 0) {
            self->records_.push_back({code, type == kCGEventKeyDown});
        }
        return event;
    }

    CFMachPortRef tap_ = nullptr;
    CFRunLoopSourceRef source_ = nullptr;
    std::vector<Record> records_;
};

class Scheduler final : public IWakeupScheduler {
public:
    explicit Scheduler(const IClock& clock) : clock_(clock) {}
    void schedule_wakeup(std::optional<Timestamp> at) override { wakeup = at; }
    // Fires the engine's timer if due (the test drives the run loop itself).
    void poll(FilterEngine& engine) {
        if (wakeup && *wakeup <= clock_.now()) {
            wakeup.reset();
            engine.on_wakeup();
        }
    }
    std::optional<Timestamp> wakeup;

private:
    const IClock& clock_;
};

void pump(double milliseconds, Scheduler& scheduler, FilterEngine& engine) {
    const CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + milliseconds / 1000.0;
    while (CFAbsoluteTimeGetCurrent() < end) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.001, true);
        scheduler.poll(engine);
    }
}

void post(CGEventSourceRef source, CGKeyCode code, bool down) {
    CGEventRef event = CGEventCreateKeyboardEvent(source, code, down);
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

}  // namespace

TEST(MacOSIntegration, ChatterIsRemovedBetweenTheTapAndApplications) {
    macos::MachClock clock;
    macos::MacKeyboardOutput output;
    Scheduler scheduler(clock);
    FilterSettings settings;  // the production macOS policy
    settings.release_mode = ReleaseMode::Immediate;
    settings.filter_modifiers = false;
    FilterEngine engine(settings, output, scheduler, clock);
    macos::MacKeyboardInterceptor interceptor(output, clock);
    interceptor.set_pass_own_events(false);  // this test posts the "hardware" events itself
    ASSERT_TRUE(interceptor.start(engine).ok());
    Recorder recorder;
    ASSERT_TRUE(recorder.ok());

    // A source in the HID system state: the tap treats these events like a physical keyboard.
    CGEventSourceRef source = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
    ASSERT_TRUE(source != nullptr);
    pump(100, scheduler, engine);

    // Release bounce on F19: one keystroke must come out.
    post(source, kF19, true);
    pump(60, scheduler, engine);
    post(source, kF19, false);
    pump(3, scheduler, engine);
    post(source, kF19, true);
    pump(3, scheduler, engine);
    post(source, kF19, false);
    pump(150, scheduler, engine);
    EXPECT_EQ(recorder.take(), (std::vector<Record>{{kF19, true}, {kF19, false}}));

    // Intentional double tap on F18, 80 ms apart: two keystrokes.
    post(source, kF18, true);
    pump(50, scheduler, engine);
    post(source, kF18, false);
    pump(80, scheduler, engine);
    post(source, kF18, true);
    pump(50, scheduler, engine);
    post(source, kF18, false);
    pump(150, scheduler, engine);
    EXPECT_EQ(recorder.take(), (std::vector<Record>{{kF18, true}, {kF18, false}, {kF18, true}, {kF18, false}}));

    EXPECT_FALSE(engine.tripped());
    interceptor.stop();
    CFRelease(source);
}

int main(int argc, char** argv) {
    if (!macos::has_accessibility_permission()) {
        std::printf("skipped: grant Accessibility access to the terminal running this test\n");
        return 77;
    }
    return kcf_test::run_all(argc, argv);
}
