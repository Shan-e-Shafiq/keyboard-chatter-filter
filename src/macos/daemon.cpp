// SPDX-License-Identifier: MIT
#include "macos/daemon.h"

#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

#include "common/logging.h"
#include "common/posix/posix.h"
#include "common/status.h"
#include "kcf/build_info.h"
#include "keyboard_filter/filter_engine.h"
#include "macos/keyboard_interceptor.h"
#include "macos/keyboard_output.h"
#include "macos/event_conversion.h"
#include "macos/mach_clock.h"
#include "macos/paths.h"

namespace kcf::macos {
namespace {

constexpr CFTimeInterval kFarFuture = 1.0e10;
constexpr CFTimeInterval kPermissionPollSeconds = 2.0;
constexpr CFTimeInterval kErrorRetrySeconds = 10.0;
constexpr int kMaxStartFailuresWithPermission = 5;
constexpr off_t kMaxStderrLogBytes = 4 * 1024 * 1024;

// One-shot timer on the main run loop that can be re-armed or cancelled cheaply.
class RunLoopTimer {
public:
    using Callback = void (*)(void* context);

    RunLoopTimer(Callback callback, void* context) : callback_(callback), context_(context) {
        CFRunLoopTimerContext ctx{0, this, nullptr, nullptr, nullptr};
        timer_.reset(CFRunLoopTimerCreate(kCFAllocatorDefault, CFAbsoluteTimeGetCurrent() + kFarFuture, kFarFuture, 0,
                                          0, &RunLoopTimer::fire, &ctx));
        CFRunLoopAddTimer(CFRunLoopGetMain(), timer_.get(), kCFRunLoopCommonModes);
    }
    ~RunLoopTimer() {
        if (timer_) {
            CFRunLoopTimerInvalidate(timer_.get());
        }
    }
    RunLoopTimer(const RunLoopTimer&) = delete;
    RunLoopTimer& operator=(const RunLoopTimer&) = delete;

    void arm_in(double seconds) {
        CFRunLoopTimerSetNextFireDate(timer_.get(), CFAbsoluteTimeGetCurrent() + (seconds > 0 ? seconds : 0));
    }
    void cancel() { CFRunLoopTimerSetNextFireDate(timer_.get(), CFAbsoluteTimeGetCurrent() + kFarFuture); }

private:
    static void fire(CFRunLoopTimerRef, void* info) {
        auto* self = static_cast<RunLoopTimer*>(info);
        self->callback_(self->context_);
    }

    CFRef<CFRunLoopTimerRef> timer_;
    Callback callback_;
    void* context_;
};

class WakeupScheduler final : public IWakeupScheduler {
public:
    WakeupScheduler(const IClock& clock, RunLoopTimer::Callback callback, void* context)
        : clock_(clock), timer_(callback, context) {}

    void schedule_wakeup(std::optional<Timestamp> at) override {
        if (!at) {
            timer_.cancel();
            return;
        }
        const auto delay = std::chrono::duration<double>(*at - clock_.now()).count();
        timer_.arm_in(delay);
    }

private:
    const IClock& clock_;
    RunLoopTimer timer_;
};

// Dispatch source on the main queue, cancelled and released on destruction.
class DispatchSource {
public:
    DispatchSource() = default;
    DispatchSource(dispatch_source_type_t type, uintptr_t handle, void* context, dispatch_function_t handler) {
        source_ = dispatch_source_create(type, handle, 0, dispatch_get_main_queue());
        if (source_ != nullptr) {
            dispatch_set_context(source_, context);
            dispatch_source_set_event_handler_f(source_, handler);
            dispatch_resume(source_);
        }
    }
    ~DispatchSource() { reset(); }
    DispatchSource(DispatchSource&& other) noexcept : source_(std::exchange(other.source_, nullptr)) {}
    DispatchSource& operator=(DispatchSource&& other) noexcept {
        if (this != &other) {
            reset();
            source_ = std::exchange(other.source_, nullptr);
        }
        return *this;
    }
    DispatchSource(const DispatchSource&) = delete;
    DispatchSource& operator=(const DispatchSource&) = delete;

    void reset() {
        if (source_ != nullptr) {
            dispatch_source_cancel(source_);
            dispatch_release(source_);
            source_ = nullptr;
        }
    }

private:
    dispatch_source_t source_ = nullptr;
};

// macOS policy: key-ups pass immediately and modifier keys are never touched, so the filter only
// ever removes complete press/release pairs and never has to inject an event into the system's
// input stream (re-injected events could leave a key or modifier stuck if anything went wrong).
FilterSettings platform_settings(const Configuration& config) {
    FilterSettings settings = config.filter_settings();
    settings.release_mode = ReleaseMode::Immediate;
    settings.filter_modifiers = false;
    return settings;
}

// launchd sets XPC_SERVICE_NAME to the job label for the processes it starts.
bool launched_by_launchd() {
    const char* service = std::getenv("XPC_SERVICE_NAME");
    return service != nullptr && std::string_view(service) == build::kServiceId;
}

class Daemon final : public IKeyEventHandler {
public:
    Daemon(DaemonOptions options, Paths paths)
        : options_(std::move(options)),
          paths_(std::move(paths)),
          scheduler_(clock_, &Daemon::on_wakeup_timer, this),
          engine_(FilterSettings{}, output_, scheduler_, clock_),
          interceptor_(output_, clock_),
          retry_timer_(&Daemon::on_retry_timer, this),
          started_(std::chrono::steady_clock::now()) {}

    int run();

    // IKeyEventHandler: forwards to the engine, watching for a tap macOS refused to re-enable and
    // for the engine's safety circuit breaker.
    Decision on_key_event(const KeyEvent& event) override {
        const Decision decision = engine_.on_key_event(event);
        if (engine_.tripped() && !reported_trip_) {
            reported_trip_ = true;
            state_ = DaemonState::Error;
            detail_ = std::string("safety stop: ") + engine_.trip_reason();
            log::error("SAFETY STOP: ", engine_.trip_reason(),
                       ". Filtering is switched off and every key passes unchanged until the filter is "
                       "restarted. Please report this.");
        }
        return decision;
    }
    void on_events_lost() override {
        engine_.on_events_lost();
        output_.clear();
        dispatch_async_f(dispatch_get_main_queue(), this, &Daemon::check_tap_health);
    }

private:
    void load_configuration(bool initial);
    void start_interception();
    void stop_interception();
    [[nodiscard]] std::string status_snapshot() const;

    static void on_wakeup_timer(void* self) { static_cast<Daemon*>(self)->engine_.on_wakeup(); }
    static void on_retry_timer(void* self) { static_cast<Daemon*>(self)->start_interception(); }
    static void on_terminate(void*) { CFRunLoopStop(CFRunLoopGetMain()); }
    static void on_reload(void* self) {
        log::info("reloading configuration");
        static_cast<Daemon*>(self)->load_configuration(false);
    }
    static void on_status_request(void* context) {
        auto* self = static_cast<Daemon*>(context);
        self->status_server_.serve_pending([self] { return self->status_snapshot(); });
    }
    static void check_tap_health(void* context) {
        auto* self = static_cast<Daemon*>(context);
        if (self->interceptor_.failed()) {
            log::warning("the event tap could not be re-enabled; recreating it");
            self->stop_interception();
            self->start_interception();
        }
    }

    DaemonOptions options_;
    Paths paths_;
    MachClock clock_;
    MacKeyboardOutput output_;
    WakeupScheduler scheduler_;
    FilterEngine engine_;
    MacKeyboardInterceptor interceptor_;
    RunLoopTimer retry_timer_;
    posix::StatusServer status_server_;
    std::vector<DispatchSource> sources_;

    Configuration config_;
    std::size_t config_problems_ = 0;
    DaemonState state_ = DaemonState::Starting;
    std::string detail_;
    bool prompted_for_permission_ = false;
    bool reported_trip_ = false;
    int start_failures_ = 0;
    int exit_code_ = 0;
    std::chrono::steady_clock::time_point started_;
};

void Daemon::load_configuration(bool initial) {
    const ConfigLoadResult result = kcf::load_configuration(options_.config_path);
    for (const std::string& line : result.describe(options_.config_path)) {
        log::warning(line);
    }
    const Configuration previous = config_;
    config_ = result.config;
    config_problems_ = result.diagnostics.size();
    log::logger().set_level(options_.log_level_override.value_or(config_.log_level));

    if (initial) {
        log::info("configuration ", result.file_found ? options_.config_path.string() : "(defaults)",
                  ": threshold ", config_.chatter_threshold.count(), " ms, ", config_.enabled ? "enabled" : "disabled");
        engine_.update_settings(platform_settings(config_));
        return;
    }
    if (config_ == previous) {
        log::info("configuration unchanged");
        return;
    }
    log::info("configuration applied: threshold ", config_.chatter_threshold.count(), " ms, ",
              config_.enabled ? "enabled" : "disabled");
    engine_.update_settings(platform_settings(config_));
    reported_trip_ = false;
    output_.clear();
    if (config_.enabled && !interceptor_.is_active()) {
        start_interception();
    } else if (!config_.enabled) {
        stop_interception();
        state_ = DaemonState::Disabled;
        detail_ = "enabled = false in the configuration";
    }
}

void Daemon::start_interception() {
    retry_timer_.cancel();
    if (!config_.enabled) {
        state_ = DaemonState::Disabled;
        detail_ = "enabled = false in the configuration";
        log::info("filtering is disabled in the configuration; not intercepting the keyboard");
        return;
    }
    if (interceptor_.is_active()) {
        return;
    }

    const InterceptorStartResult result = interceptor_.start(*this);
    if (result.ok()) {
        const bool was_waiting = state_ == DaemonState::WaitingForPermission;
        state_ = DaemonState::Active;
        detail_ = options_.dry_run ? "dry run" : "";
        start_failures_ = 0;
        log::info(was_waiting ? "Accessibility permission granted; " : "",
                  options_.dry_run ? "dry run active: observing only, nothing is dropped" : "keyboard filtering active");
        return;
    }

    if (result.error == InterceptorError::PermissionDenied) {
        if (state_ != DaemonState::WaitingForPermission) {
            const auto exe = posix::current_executable();
            const std::string exe_path = exe ? exe->string() : std::string(build::kProgramName);
            log::warning(result.message, ". Open System Settings > Privacy & Security > Accessibility and switch on '",
                         build::kProgramName, "' (", exe_path,
                         "). If it is not listed, add it with the '+' button. When running from a terminal, the "
                         "terminal application needs the permission instead. Filtering starts automatically once "
                         "access is granted; until then the keyboard works normally, unfiltered.");
        }
        state_ = DaemonState::WaitingForPermission;
        detail_ = "grant Accessibility access in System Settings > Privacy & Security > Accessibility";
        if (!prompted_for_permission_ && launched_by_launchd()) {
            // Only the LaunchAgent prompts: from a terminal the request would name the terminal
            // application, which is confusing; the log message above explains what to do instead.
            prompted_for_permission_ = true;
            request_accessibility_permission();
        }
        retry_timer_.arm_in(kPermissionPollSeconds);
        return;
    }

    ++start_failures_;
    state_ = DaemonState::Error;
    detail_ = result.message;
    log::error("cannot intercept keyboard events: ", result.message);
    if (has_accessibility_permission() && start_failures_ >= kMaxStartFailuresWithPermission) {
        // Some macOS versions only honour a newly granted permission in a fresh process. Exit with
        // an error so launchd starts a new instance.
        log::error("giving up after ", start_failures_, " attempts; exiting so the service manager restarts us");
        CFRunLoopStop(CFRunLoopGetMain());
        exit_code_ = 1;
        return;
    }
    retry_timer_.arm_in(kErrorRetrySeconds);
}

void Daemon::stop_interception() {
    retry_timer_.cancel();
    // Release anything held back before the tap disappears, so no key is left pressed.
    engine_.flush();
    interceptor_.stop();
    output_.clear();
}

std::string Daemon::status_snapshot() const {
    DaemonStatus s;
    s.version = build::kVersion;
    s.pid = ::getpid();
    s.state = state_;
    s.detail = detail_;
    s.threshold_ms = config_.chatter_threshold.count();
    s.enabled = config_.enabled;
    s.devices = -1;  // event taps see all keyboards merged
    const FilterStats& stats = engine_.filter().stats();
    s.suppressed = stats.chatter_suppressed;
    for (int i = 0; i < 4; ++i) {
        s.chatter_gaps[i] = stats.chatter_gap_histogram[i];
    }
    s.dry_run = options_.dry_run;
    if (engine_.tripped()) {
        s.safety_stop = engine_.trip_reason();
    }
    for (const TimestampSource source :
         {TimestampSource::EventNanoseconds, TimestampSource::EventTicks, TimestampSource::Arrival}) {
        if (const auto n = interceptor_.timestamp_source_count(source); n > 0) {
            s.timing += (s.timing.empty() ? "" : ", ") + std::string(to_string(source)) + " x" + std::to_string(n);
        }
    }
    s.config_path = options_.config_path.string();
    s.config_problems = static_cast<std::int64_t>(config_problems_);
    s.uptime_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
    return serialize_status(s);
}

int Daemon::run() {
    ::umask(077);

    std::string error;
    if (!posix::ensure_directory(paths_.state_dir, 0700, error)) {
        log::error(error);
        return 1;
    }
    auto lock = posix::InstanceLock::acquire(paths_.lock_file, error);
    if (!lock) {
        log::error("not starting: ", error);
        return 3;
    }

    log::info(build::kProgramName, ' ', build::kVersion, " starting (pid ", ::getpid(), ")",
              options_.dry_run ? " in dry-run mode" : "");
    interceptor_.set_dry_run(options_.dry_run);
    output_.set_dry_run(options_.dry_run);
    load_configuration(true);

    if (!status_server_.open(paths_.status_socket, 0600, error)) {
        log::warning("status socket unavailable: ", error);
    } else {
        sources_.emplace_back(DISPATCH_SOURCE_TYPE_READ, static_cast<uintptr_t>(status_server_.fd()), this,
                              &Daemon::on_status_request);
    }

    std::signal(SIGTERM, SIG_IGN);
    std::signal(SIGINT, SIG_IGN);
    std::signal(SIGHUP, SIG_IGN);
    std::signal(SIGPIPE, SIG_IGN);
    sources_.emplace_back(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, this, &Daemon::on_terminate);
    sources_.emplace_back(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, this, &Daemon::on_terminate);
    sources_.emplace_back(DISPATCH_SOURCE_TYPE_SIGNAL, SIGHUP, this, &Daemon::on_reload);

    start_interception();
    CFRunLoopRun();

    log::info("stopping");
    stop_interception();
    sources_.clear();
    status_server_.close();
    log::info("stopped");
    return exit_code_;
}

// Keeps the log written through launchd's StandardErrorPath bounded. launchd opens it in append
// mode, so truncating in place is safe.
void bound_stderr_log() {
    struct stat st {};
    if (::fstat(STDERR_FILENO, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > kMaxStderrLogBytes) {
        if (::ftruncate(STDERR_FILENO, 0) == 0) {
            log::info("log file exceeded ", kMaxStderrLogBytes / (1024 * 1024), " MiB and was truncated");
        }
    }
}

}  // namespace

int run_daemon(const DaemonOptions& options) {
    const std::optional<Paths> paths = user_paths();
    if (!paths) {
        log::error("cannot determine the home directory");
        return 1;
    }
    bound_stderr_log();
    Daemon daemon(options, *paths);
    return daemon.run();
}

}  // namespace kcf::macos
