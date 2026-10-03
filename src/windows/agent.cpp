// SPDX-License-Identifier: MIT
//
// The per-session agent: owns the low-level keyboard hook, the filter and the timer, all on one
// thread that pumps messages as WH_KEYBOARD_LL requires. Launched by the service in every
// interactive session (low-level hooks only see input of the desktop they run on, so the Session 0
// service cannot hook the user's keyboard itself), or directly with `keyboard-chatter-filter run`.
#include "windows/agent.h"

#include <wtsapi32.h>

#include <chrono>
#include <vector>

#include "common/logging.h"
#include "kcf/build_info.h"
#include "keyboard_filter/filter_engine.h"
#include "windows/keyboard_interceptor.h"
#include "windows/keyboard_output.h"
#include "windows/win_util.h"

namespace kcf::windows {
namespace {

HANDLE g_console_stop = nullptr;  // for the console control handler (runs on another thread)

BOOL WINAPI console_handler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT ||
        type == CTRL_SHUTDOWN_EVENT || type == CTRL_LOGOFF_EVENT) {
        if (g_console_stop != nullptr) {
            ::SetEvent(g_console_stop);
        }
        return TRUE;
    }
    return FALSE;
}

// Windows policy: key-ups pass immediately and modifier keys are never touched, so the filter only
// ever blocks complete press/release pairs and never re-injects events with SendInput (re-injected
// input could leave a key stuck, and some software ignores injected input).
FilterSettings platform_settings(const Configuration& config) {
    FilterSettings settings = config.filter_settings();
    settings.release_mode = ReleaseMode::Immediate;
    settings.filter_modifiers = false;
    return settings;
}

class WaitableTimerScheduler final : public IWakeupScheduler {
public:
    explicit WaitableTimerScheduler(const IClock& clock) : clock_(clock) {
        timer_.reset(::CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                              TIMER_ALL_ACCESS));
        if (!timer_) {
            // Before Windows 10 1803: a regular timer (~1-16 ms granularity) still works.
            timer_.reset(::CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS));
        }
    }
    void schedule_wakeup(std::optional<Timestamp> at) override {
        if (!timer_) {
            return;
        }
        if (!at) {
            ::CancelWaitableTimer(timer_.get());
            return;
        }
        const auto delay = std::max<std::int64_t>((*at - clock_.now()).count(), 0);
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<std::int64_t>(delay / 100, 1);  // relative, in 100 ns units
        ::SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE);
    }
    [[nodiscard]] HANDLE handle() const noexcept { return timer_.get(); }

private:
    const IClock& clock_;
    UniqueHandle timer_;
};

class Agent {
public:
    explicit Agent(const AgentOptions& options)
        : options_(options), scheduler_(clock_), engine_(FilterSettings{}, output_, scheduler_, clock_),
          interceptor_(output_, clock_) {}

    int run();

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    void load_configuration(bool initial);
    void start_hook();
    void restart_hook(const char* why);
    void stop_hook();
    bool create_window();

    AgentOptions options_;
    QpcClock clock_;
    WinKeyboardOutput output_;
    WaitableTimerScheduler scheduler_;
    FilterEngine engine_;
    WinKeyboardInterceptor interceptor_;
    Configuration config_;
    HWND window_ = nullptr;
    bool quit_ = false;
    bool reported_trip_ = false;
};

void Agent::load_configuration(bool initial) {
    const ConfigLoadResult result = kcf::load_configuration(options_.config_path);
    for (const std::string& line : result.describe(options_.config_path)) {
        log::warning(line);
    }
    const Configuration previous = config_;
    config_ = result.config;
    log::logger().set_level(options_.log_level_override.value_or(config_.log_level));
    if (!initial && config_ == previous) {
        log::info("configuration unchanged");
        return;
    }
    log::info("configuration ", initial ? "" : "applied ", result.file_found ? "" : "(defaults) ", ": threshold ",
              config_.chatter_threshold.count(), " ms, ", config_.enabled ? "enabled" : "disabled");
    engine_.update_settings(platform_settings(config_));
    reported_trip_ = false;
    output_.send_queued();
    if (config_.enabled && !interceptor_.is_active()) {
        start_hook();
    } else if (!config_.enabled && interceptor_.is_active()) {
        stop_hook();
    }
}

void Agent::start_hook() {
    const InterceptorStartResult result = interceptor_.start(engine_);
    if (result.ok()) {
        log::info("keyboard filtering active");
    } else {
        log::error("cannot intercept keyboard events: ", result.message);
    }
}

void Agent::stop_hook() {
    engine_.flush();
    output_.send_queued();
    interceptor_.stop();
}

// Windows silently removes low-level hooks that ever time out, and events during a lock screen or
// sleep never reach us. Re-installing at these points keeps the filter alive and its state fresh.
void Agent::restart_hook(const char* why) {
    if (!config_.enabled) {
        return;
    }
    log::debug("reinstalling keyboard hook (", why, ")");
    engine_.on_events_lost();
    output_.send_queued();
    interceptor_.stop();
    start_hook();
}

LRESULT CALLBACK Agent::window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<Agent*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (self != nullptr) {
        switch (message) {
            case WM_WTSSESSION_CHANGE:
                if (wparam == WTS_SESSION_UNLOCK) {
                    self->restart_hook("session unlocked");
                }
                return 0;
            case WM_POWERBROADCAST:
                if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
                    self->restart_hook("resumed from sleep");
                }
                return TRUE;
            case WM_QUERYENDSESSION:
                return TRUE;
            case WM_ENDSESSION:
                if (wparam != 0) {
                    self->stop_hook();
                    self->quit_ = true;
                }
                return 0;
            case WM_CLOSE:
                self->quit_ = true;
                return 0;
            default:
                break;
        }
    }
    return ::DefWindowProcW(hwnd, message, wparam, lparam);
}

bool Agent::create_window() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof wc;
    wc.lpfnWndProc = &Agent::window_proc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    wc.lpszClassName = L"KeyboardChatterFilterAgent";
    ::RegisterClassExW(&wc);
    // A hidden top-level window (never shown): message-only windows receive neither power
    // broadcasts nor end-session messages.
    window_ = ::CreateWindowExW(0, wc.lpszClassName, L"keyboard-chatter-filter", WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                nullptr, wc.hInstance, this);
    if (window_ == nullptr) {
        log::warning("cannot create the notification window: ", last_error_message());
        return false;
    }
    ::WTSRegisterSessionNotification(window_, NOTIFY_FOR_THIS_SESSION);
    return true;
}

int Agent::run() {
    UniqueHandle instance(::CreateMutexW(nullptr, TRUE, kAgentMutexName));
    if (::GetLastError() == ERROR_ALREADY_EXISTS) {
        log::error("not starting: the filter is already running in this session");
        return kAgentExitAlreadyRunning;
    }

    UniqueHandle console_stop;
    HANDLE stop = options_.stop_event;
    if (stop == nullptr) {
        console_stop.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        stop = console_stop.get();
        g_console_stop = stop;
        ::SetConsoleCtrlHandler(&console_handler, TRUE);
    }

    log::info(build::kProgramName, ' ', build::kVersion, " starting (pid ", ::GetCurrentProcessId(), ")",
              options_.dry_run ? " in dry-run mode" : "");
    interceptor_.set_dry_run(options_.dry_run);
    create_window();
    load_configuration(true);

    std::vector<HANDLE> handles{stop};
    if (scheduler_.handle() != nullptr) {
        handles.push_back(scheduler_.handle());
    }
    const std::size_t reload_index = handles.size();
    if (options_.reload_event != nullptr) {
        handles.push_back(options_.reload_event);
    }

    while (!quit_) {
        const DWORD result = ::MsgWaitForMultipleObjectsEx(static_cast<DWORD>(handles.size()), handles.data(),
                                                           INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (result == WAIT_OBJECT_0) {
            break;  // stop requested
        }
        if (result == WAIT_FAILED) {
            log::error("wait failed: ", last_error_message());
            break;
        }
        const DWORD index = result - WAIT_OBJECT_0;
        if (index < handles.size()) {
            if (handles[index] == scheduler_.handle()) {
                engine_.on_wakeup();
            } else if (index == reload_index) {
                log::info("reloading configuration");
                load_configuration(false);
            }
        }
        // Pump messages: this is where the system runs our low-level hook procedure.
        MSG msg;
        while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                quit_ = true;
                break;
            }
            ::TranslateMessage(&msg);
            ::DispatchMessageW(&msg);
        }
        output_.send_queued();
        if (engine_.tripped() && !reported_trip_) {
            reported_trip_ = true;
            log::error("SAFETY STOP: ", engine_.trip_reason(),
                       ". Filtering is switched off and every key passes unchanged until the filter is "
                       "restarted. Please report this.");
        }
    }

    log::info("stopping");
    stop_hook();
    if (window_ != nullptr) {
        ::WTSUnRegisterSessionNotification(window_);
        ::DestroyWindow(window_);
    }
    g_console_stop = nullptr;
    log::info("stopped");
    return kAgentExitOk;
}

}  // namespace

int run_agent(const AgentOptions& options) {
    Agent agent(options);
    return agent.run();
}

}  // namespace kcf::windows
