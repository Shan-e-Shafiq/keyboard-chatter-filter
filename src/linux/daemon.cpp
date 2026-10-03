// SPDX-License-Identifier: MIT
#include "linux/daemon.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <sys/epoll.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <unistd.h>

#include "common/logging.h"
#include "common/posix/posix.h"
#include "common/status.h"
#include "kcf/build_info.h"
#include "keyboard_filter/filter_engine.h"
#include "linux/device_classifier.h"
#include "linux/input_device.h"
#include "linux/keyboard_interceptor.h"
#include "linux/keyboard_output.h"

namespace kcf::linux_input {

RuntimePaths system_runtime_paths() {
    RuntimePaths p;
    p.dir = std::filesystem::path("/run") / build::kServiceId;
    p.lock_file = p.dir / "daemon.lock";
    p.status_socket = p.dir / "status.sock";
    p.system_wide = true;
    return p;
}

RuntimePaths runtime_paths_for_current_user() {
    if (::geteuid() == 0) {
        return system_runtime_paths();
    }
    RuntimePaths p;
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    if (runtime != nullptr && runtime[0] == '/') {
        p.dir = std::filesystem::path(runtime) / build::kProgramName;
    } else {
        p.dir = std::filesystem::path("/tmp") / (std::string(build::kProgramName) + "-" + std::to_string(::getuid()));
    }
    p.lock_file = p.dir / "daemon.lock";
    p.status_socket = p.dir / "status.sock";
    return p;
}

namespace {

constexpr int kRetrySeconds = 10;

enum class Source : std::uint32_t { Inotify = 1, Signal, FilterTimer, Watchdog, Status, Device, Uinput, Retry };

std::uint64_t tag(Source source, std::uint32_t id = 0) {
    return (static_cast<std::uint64_t>(source) << 32) | id;
}

class MonotonicClock final : public IClock {
public:
    [[nodiscard]] Timestamp now() const noexcept override {
        timespec ts{};
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        return Timestamp(static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec);
    }
};

// sd_notify(3) without libsystemd: one datagram to $NOTIFY_SOCKET.
void notify_systemd(const std::string& message) {
    const char* path = std::getenv("NOTIFY_SOCKET");
    if (path == nullptr || (path[0] != '/' && path[0] != '@')) {
        return;
    }
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const std::size_t length = std::strlen(path);
    if (length >= sizeof addr.sun_path) {
        return;
    }
    std::memcpy(addr.sun_path, path, length);
    if (addr.sun_path[0] == '@') {
        addr.sun_path[0] = '\0';  // abstract namespace
    }
    posix::UniqueFd fd(::socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0));
    if (!fd) {
        return;
    }
    const auto addr_length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + length);
    ::sendto(fd.get(), message.data(), message.size(), MSG_NOSIGNAL, reinterpret_cast<const sockaddr*>(&addr),
             addr_length);
}

class SessionScheduler final : public IWakeupScheduler {
public:
    explicit SessionScheduler(bool& dirty) : dirty_(dirty) {}
    void schedule_wakeup(std::optional<Timestamp> at) override {
        wakeup = at;
        dirty_ = true;
    }
    std::optional<Timestamp> wakeup;

private:
    bool& dirty_;
};

// Everything that exists for one physical keyboard we filter. Members are destroyed in reverse
// order: the grab is released first, the virtual keyboard removed last.
struct Session {
    std::filesystem::path path;
    dev_t rdev = 0;
    std::string name;
    std::unique_ptr<UinputKeyboardOutput> output;
    std::unique_ptr<SessionScheduler> scheduler;
    std::unique_ptr<FilterEngine> engine;
    std::unique_ptr<EvdevKeyboard> keyboard;
};

class Daemon {
public:
    Daemon(DaemonOptions options, RuntimePaths paths)
        : options_(std::move(options)), paths_(std::move(paths)), started_(std::chrono::steady_clock::now()) {}
    ~Daemon() { remove_all_sessions(); }
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    int run();

private:
    bool setup(std::string& error);
    void load_configuration(bool initial);
    void scan_devices();
    void consider_device(const std::filesystem::path& path);
    void add_session(EvdevDevice device);
    void remove_session(std::uint32_t id, const char* why);
    void remove_all_sessions();
    void flush_other_sessions(std::uint32_t except);
    void handle_inotify();
    void handle_signals(bool& running);
    void handle_filter_timer();
    void rearm_filter_timer();
    void arm_retry();
    void update_state();
    [[nodiscard]] std::string status_snapshot() const;
    bool watch(int fd, std::uint64_t data);
    void unwatch(int fd);

    DaemonOptions options_;
    RuntimePaths paths_;
    MonotonicClock clock_;
    Configuration config_;
    std::size_t config_problems_ = 0;

    posix::UniqueFd epoll_;
    posix::UniqueFd inotify_;
    posix::UniqueFd signal_;
    posix::UniqueFd filter_timer_;
    posix::UniqueFd watchdog_timer_;
    posix::UniqueFd retry_timer_;
    posix::StatusServer status_server_;

    std::map<std::uint32_t, Session> sessions_;
    std::set<dev_t> reported_;
    std::uint32_t next_id_ = 1;
    bool timer_dirty_ = false;
    bool uinput_failed_ = false;
    std::string uinput_error_;
    bool permission_warned_ = false;
    std::uint64_t suppressed_by_closed_sessions_ = 0;
    DaemonState state_ = DaemonState::Starting;
    std::string detail_;
    std::chrono::steady_clock::time_point started_;
};

bool Daemon::watch(int fd, std::uint64_t data) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = data;
    return ::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &ev) == 0;
}

void Daemon::unwatch(int fd) {
    if (fd >= 0) {
        ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
    }
}

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
        return;
    }
    if (config_ == previous) {
        log::info("configuration unchanged");
        return;
    }
    log::info("configuration applied: threshold ", config_.chatter_threshold.count(), " ms, ",
              config_.enabled ? "enabled" : "disabled");
    reported_.clear();
    if (!config_.enabled) {
        remove_all_sessions();
        update_state();
        return;
    }
    std::vector<std::uint32_t> now_ignored;
    for (auto& [id, session] : sessions_) {
        session.engine->update_settings(config_.filter_settings());
        const Classification c = classify_device(session.keyboard->device().capabilities(), config_.ignored_devices);
        if (c.kind != DeviceKind::Keyboard) {
            now_ignored.push_back(id);
        }
    }
    for (const std::uint32_t id : now_ignored) {
        remove_session(id, "now excluded by the configuration");
    }
    scan_devices();
    update_state();
}

void Daemon::scan_devices() {
    if (!config_.enabled) {
        return;
    }
    std::error_code ec;
    std::vector<std::filesystem::path> nodes;
    for (const auto& entry : std::filesystem::directory_iterator(options_.input_dir, ec)) {
        if (entry.path().filename().string().rfind("event", 0) == 0) {
            nodes.push_back(entry.path());
        }
    }
    std::sort(nodes.begin(), nodes.end());
    for (const auto& node : nodes) {
        consider_device(node);
    }
}

void Daemon::consider_device(const std::filesystem::path& path) {
    if (!config_.enabled || path.filename().string().rfind("event", 0) != 0) {
        return;
    }
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0 || !S_ISCHR(st.st_mode)) {
        return;
    }
    for (const auto& [id, session] : sessions_) {
        if (session.rdev == st.st_rdev) {
            return;  // already ours
        }
    }

    std::string error;
    std::optional<EvdevDevice> device = EvdevDevice::open(path, error);
    if (!device) {
        if ((errno == EACCES || errno == EPERM) && !permission_warned_) {
            permission_warned_ = true;
            log::error("permission denied opening ", path.string(),
                       ": the filter needs root to read keyboards (the installed systemd service runs as root)");
        }
        return;
    }
    const DeviceCapabilities& caps = device->capabilities();
    if (options_.only_device_name && caps.name != *options_.only_device_name) {
        return;
    }
    const Classification c = classify_device(caps, config_.ignored_devices);
    const bool first_report = reported_.insert(st.st_rdev).second;
    if (c.kind == DeviceKind::NotKeyboard) {
        if (first_report) {
            log::debug("skipping ", path.string(), " \"", caps.name, "\": ", c.reason);
        }
        return;
    }
    if (c.kind == DeviceKind::Ignored) {
        if (first_report && c.reason != "our own filtered keyboard") {
            log::info("not filtering ", path.string(), " \"", caps.name, "\": ", c.reason);
        }
        return;
    }
    add_session(std::move(*device));
}

void Daemon::add_session(EvdevDevice device) {
    std::string error;
    std::optional<UinputDevice> uinput = UinputDevice::create(device.capabilities(), error);
    if (!uinput) {
        if (!uinput_failed_) {
            log::error("cannot create the filtered virtual keyboard: ", error,
                       ". Is the uinput kernel module available (modprobe uinput) and is the filter running as "
                       "root? Retrying every ", kRetrySeconds, " s; the keyboard keeps working unfiltered.");
        }
        uinput_failed_ = true;
        uinput_error_ = error;
        arm_retry();
        update_state();
        return;
    }

    const std::uint32_t id = next_id_++;
    Session session;
    session.path = device.path();
    session.rdev = device.device_number();
    session.name = device.capabilities().name;
    session.output = std::make_unique<UinputKeyboardOutput>(std::move(*uinput));
    session.scheduler = std::make_unique<SessionScheduler>(timer_dirty_);
    session.engine = std::make_unique<FilterEngine>(config_.filter_settings(), *session.output, *session.scheduler,
                                                    clock_);
    session.keyboard = std::make_unique<EvdevKeyboard>(std::move(device), *session.output, id,
                                                       [this, id] { flush_other_sessions(id); });

    const InterceptorStartResult started = session.keyboard->start(*session.engine);
    if (!started.ok()) {
        log::warning("cannot filter ", session.path.string(), " \"", session.name, "\": ", started.message);
        return;  // the session (and its virtual keyboard) is discarded
    }
    if (!watch(session.keyboard->device().fd(), tag(Source::Device, id)) ||
        !watch(session.output->device().fd(), tag(Source::Uinput, id))) {
        log::error("epoll_ctl failed: ", posix::errno_message(errno));
        unwatch(session.keyboard->device().fd());
        return;
    }
    if (session.keyboard->waiting_for_release()) {
        log::info("found \"", session.name, "\" (", session.path.string(),
                  "); filtering starts once all its keys are released");
    } else {
        log::info("filtering \"", session.name, "\" (", session.path.string(), ")");
    }
    uinput_failed_ = false;
    sessions_.emplace(id, std::move(session));
    update_state();
}

void Daemon::remove_session(std::uint32_t id, const char* why) {
    const auto it = sessions_.find(id);
    if (it == sessions_.end()) {
        return;
    }
    Session& session = it->second;
    unwatch(session.keyboard->device().fd());
    unwatch(session.output->device().fd());
    // Deliver anything held back while the virtual keyboard still exists, then let go.
    session.engine->flush();
    session.keyboard->stop();
    suppressed_by_closed_sessions_ += session.engine->filter().stats().chatter_suppressed;
    log::info("stopped filtering \"", session.name, "\" (", why, ")");
    sessions_.erase(it);
    timer_dirty_ = true;
    update_state();
}

void Daemon::remove_all_sessions() {
    while (!sessions_.empty()) {
        remove_session(sessions_.begin()->first, "shutting down");
    }
}

void Daemon::flush_other_sessions(std::uint32_t except) {
    for (auto& [id, session] : sessions_) {
        if (id != except && session.engine->filter().has_pending()) {
            session.engine->flush();
        }
    }
}

void Daemon::handle_inotify() {
    alignas(inotify_event) char buffer[4096];
    while (true) {
        const ssize_t n = ::read(inotify_.get(), buffer, sizeof buffer);
        if (n <= 0) {
            return;
        }
        for (char* p = buffer; p < buffer + n;) {
            const auto* ev = reinterpret_cast<const inotify_event*>(p);
            if ((ev->mask & IN_Q_OVERFLOW) != 0) {
                scan_devices();
            } else if (ev->len > 0) {
                consider_device(options_.input_dir / ev->name);
            }
            p += sizeof(inotify_event) + ev->len;
        }
    }
}

void Daemon::handle_signals(bool& running) {
    signalfd_siginfo info{};
    while (::read(signal_.get(), &info, sizeof info) == static_cast<ssize_t>(sizeof info)) {
        if (info.ssi_signo == SIGHUP) {
            log::info("reloading configuration");
            notify_systemd("RELOADING=1");
            load_configuration(false);
            notify_systemd("READY=1");
        } else {
            running = false;
        }
    }
}

void Daemon::handle_filter_timer() {
    std::uint64_t expirations = 0;
    [[maybe_unused]] const ssize_t n = ::read(filter_timer_.get(), &expirations, sizeof expirations);
    const Timestamp now = clock_.now();
    for (auto& [id, session] : sessions_) {
        if (session.scheduler->wakeup && *session.scheduler->wakeup <= now) {
            session.engine->on_wakeup();
        }
    }
    timer_dirty_ = true;
}

void Daemon::rearm_filter_timer() {
    timer_dirty_ = false;
    std::optional<Timestamp> earliest;
    for (const auto& [id, session] : sessions_) {
        const auto& wake = session.scheduler->wakeup;
        if (wake && (!earliest || *wake < *earliest)) {
            earliest = wake;
        }
    }
    itimerspec spec{};
    if (earliest) {
        const std::int64_t ns = std::max<std::int64_t>(earliest->count(), 1);
        spec.it_value.tv_sec = static_cast<time_t>(ns / 1'000'000'000);
        spec.it_value.tv_nsec = static_cast<long>(ns % 1'000'000'000);
    }
    ::timerfd_settime(filter_timer_.get(), TFD_TIMER_ABSTIME, &spec, nullptr);
}

void Daemon::arm_retry() {
    itimerspec spec{};
    spec.it_value.tv_sec = kRetrySeconds;
    ::timerfd_settime(retry_timer_.get(), 0, &spec, nullptr);
}

void Daemon::update_state() {
    if (!config_.enabled) {
        state_ = DaemonState::Disabled;
        detail_ = "enabled = false in the configuration";
    } else if (uinput_failed_ && sessions_.empty()) {
        state_ = DaemonState::Error;
        detail_ = "cannot create the virtual keyboard: " + uinput_error_;
    } else {
        state_ = DaemonState::Active;
        detail_ = sessions_.empty() ? "no keyboard found yet; waiting for one to be connected" : "";
    }
}

std::string Daemon::status_snapshot() const {
    DaemonStatus s;
    s.version = build::kVersion;
    s.pid = ::getpid();
    s.state = state_;
    s.detail = detail_;
    s.threshold_ms = config_.chatter_threshold.count();
    s.enabled = config_.enabled;
    s.devices = static_cast<std::int64_t>(sessions_.size());
    s.suppressed = suppressed_by_closed_sessions_;
    for (const auto& [id, session] : sessions_) {
        s.suppressed += session.engine->filter().stats().chatter_suppressed;
    }
    s.config_path = options_.config_path.string();
    s.config_problems = static_cast<std::int64_t>(config_problems_);
    s.uptime_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count();
    return serialize_status(s);
}

bool Daemon::setup(std::string& error) {
    epoll_.reset(::epoll_create1(EPOLL_CLOEXEC));
    if (!epoll_) {
        error = "epoll_create1: " + posix::errno_message(errno);
        return false;
    }

    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGTERM);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGHUP);
    ::sigprocmask(SIG_BLOCK, &signals, nullptr);
    std::signal(SIGPIPE, SIG_IGN);
    signal_.reset(::signalfd(-1, &signals, SFD_NONBLOCK | SFD_CLOEXEC));
    filter_timer_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    retry_timer_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
    inotify_.reset(::inotify_init1(IN_NONBLOCK | IN_CLOEXEC));
    if (!signal_ || !filter_timer_ || !retry_timer_ || !inotify_) {
        error = "cannot create event sources: " + posix::errno_message(errno);
        return false;
    }
    if (::inotify_add_watch(inotify_.get(), options_.input_dir.c_str(), IN_CREATE | IN_ATTRIB | IN_MOVED_TO) < 0) {
        log::warning("cannot watch ", options_.input_dir.string(), " for new keyboards: ", posix::errno_message(errno));
    }
    watch(signal_.get(), tag(Source::Signal));
    watch(filter_timer_.get(), tag(Source::FilterTimer));
    watch(retry_timer_.get(), tag(Source::Retry));
    watch(inotify_.get(), tag(Source::Inotify));

    std::string socket_error;
    if (status_server_.open(paths_.status_socket, paths_.system_wide ? 0666 : 0600, socket_error)) {
        watch(status_server_.fd(), tag(Source::Status));
    } else {
        log::warning("status socket unavailable: ", socket_error);
    }

    // systemd watchdog: if the loop ever hangs while holding the keyboards, systemd kills the
    // process and the kernel hands the keyboards straight back.
    const char* watchdog_usec = std::getenv("WATCHDOG_USEC");
    const char* watchdog_pid = std::getenv("WATCHDOG_PID");
    if (watchdog_usec != nullptr &&
        (watchdog_pid == nullptr || std::strtol(watchdog_pid, nullptr, 10) == static_cast<long>(::getpid()))) {
        const long long usec = std::strtoll(watchdog_usec, nullptr, 10);
        if (usec > 0) {
            watchdog_timer_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
            itimerspec spec{};
            const long long half = usec / 2;
            spec.it_interval.tv_sec = static_cast<time_t>(half / 1'000'000);
            spec.it_interval.tv_nsec = static_cast<long>((half % 1'000'000) * 1000);
            spec.it_value = spec.it_interval;
            ::timerfd_settime(watchdog_timer_.get(), 0, &spec, nullptr);
            watch(watchdog_timer_.get(), tag(Source::Watchdog));
        }
    }
    return true;
}

int Daemon::run() {
    ::umask(077);
    std::string error;
    if (!posix::ensure_directory(paths_.dir, paths_.system_wide ? 0755 : 0700, error)) {
        log::error(error);
        return 1;
    }
    auto lock = posix::InstanceLock::acquire(paths_.lock_file, error);
    if (!lock) {
        log::error("not starting: ", error);
        return 3;
    }
    log::info(build::kProgramName, ' ', build::kVersion, " starting (pid ", ::getpid(), ")");
    load_configuration(true);
    if (!setup(error)) {
        log::error(error);
        return 1;
    }

    scan_devices();
    update_state();
    if (config_.enabled && sessions_.empty() && !uinput_failed_) {
        log::info("no keyboard found yet; waiting for one to be connected");
    }
    notify_systemd("READY=1\nSTATUS=filtering " + std::to_string(sessions_.size()) + " keyboard(s)");

    bool running = true;
    epoll_event events[32];
    while (running) {
        if (timer_dirty_) {
            rearm_filter_timer();
        }
        const int n = ::epoll_wait(epoll_.get(), events, 32, -1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            log::error("epoll_wait: ", posix::errno_message(errno));
            break;
        }
        for (int i = 0; i < n; ++i) {
            const auto source = static_cast<Source>(events[i].data.u64 >> 32);
            const auto id = static_cast<std::uint32_t>(events[i].data.u64 & 0xFFFFFFFFu);
            switch (source) {
                case Source::Device: {
                    const auto it = sessions_.find(id);
                    if (it == sessions_.end()) {
                        break;
                    }
                    const bool hangup = (events[i].events & (EPOLLERR | EPOLLHUP)) != 0;
                    if (it->second.keyboard->on_readable() == EvdevKeyboard::ReadStatus::Gone || hangup) {
                        remove_session(id, "disconnected");
                    }
                    break;
                }
                case Source::Uinput: {
                    const auto it = sessions_.find(id);
                    if (it != sessions_.end()) {
                        it->second.keyboard->on_virtual_led_feedback();
                    }
                    break;
                }
                case Source::Inotify: handle_inotify(); break;
                case Source::Signal: handle_signals(running); break;
                case Source::FilterTimer: handle_filter_timer(); break;
                case Source::Watchdog: {
                    std::uint64_t expirations = 0;
                    [[maybe_unused]] const ssize_t r = ::read(watchdog_timer_.get(), &expirations, sizeof expirations);
                    notify_systemd("WATCHDOG=1");
                    break;
                }
                case Source::Status:
                    status_server_.serve_pending([this] { return status_snapshot(); });
                    break;
                case Source::Retry: {
                    std::uint64_t expirations = 0;
                    [[maybe_unused]] const ssize_t r = ::read(retry_timer_.get(), &expirations, sizeof expirations);
                    uinput_failed_ = false;
                    scan_devices();
                    update_state();
                    break;
                }
            }
        }
    }

    notify_systemd("STOPPING=1");
    log::info("stopping");
    // Deliver held-back releases first and give readers a moment to consume them before the virtual
    // keyboards disappear, so no application is left believing a key is still down.
    bool flushed = false;
    for (auto& [id, session] : sessions_) {
        flushed = flushed || session.engine->filter().has_pending();
        session.engine->flush();
    }
    if (flushed) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    remove_all_sessions();
    status_server_.close();
    log::info("stopped");
    return 0;
}

}  // namespace

int run_daemon(const DaemonOptions& options) {
    Daemon daemon(options, runtime_paths_for_current_user());
    return daemon.run();
}

}  // namespace kcf::linux_input
