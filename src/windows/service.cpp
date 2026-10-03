// SPDX-License-Identifier: MIT
#include "windows/service.h"

#include <userenv.h>
#include <wtsapi32.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <vector>

#include "common/logging.h"
#include "kcf/build_info.h"
#include "windows/agent.h"
#include "windows/win_util.h"

namespace kcf::windows {
namespace {

using Clock = std::chrono::steady_clock;

constexpr auto kMinRestartDelay = std::chrono::seconds(1);
constexpr auto kMaxRestartDelay = std::chrono::seconds(60);
constexpr auto kStableRun = std::chrono::minutes(5);
constexpr DWORD kAgentStopTimeoutMs = 3000;

struct AgentProcess {
    UniqueHandle process;
    UniqueHandle stop_event;
    UniqueHandle reload_event;
    Clock::time_point started{};
    Clock::time_point restart_at{};  // when a failed agent may be started again
    std::chrono::seconds backoff = kMinRestartDelay;
    bool running = false;
};

class Supervisor {
public:
    Supervisor() {
        stop_.reset(::CreateEventW(nullptr, TRUE, FALSE, nullptr));
        wake_.reset(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    }

    // Called on the SCM dispatcher thread.
    void request_stop() { ::SetEvent(stop_.get()); }
    void request_reload() {
        std::lock_guard lock(mutex_);
        reload_requested_ = true;
        ::SetEvent(wake_.get());
    }
    void session_changed(DWORD event, DWORD session) {
        std::lock_guard lock(mutex_);
        session_events_.emplace_back(event, session);
        ::SetEvent(wake_.get());
    }

    void run();

private:
    void ensure_agent(DWORD session);
    bool launch(DWORD session, AgentProcess& agent);
    void reap(DWORD session, AgentProcess& agent);
    void stop_all();
    void process_requests();

    UniqueHandle stop_;
    UniqueHandle wake_;
    std::mutex mutex_;
    std::deque<std::pair<DWORD, DWORD>> session_events_;
    bool reload_requested_ = false;
    std::map<DWORD, AgentProcess> agents_;
};

bool Supervisor::launch(DWORD session, AgentProcess& agent) {
    HANDLE user_token = nullptr;
    if (!::WTSQueryUserToken(session, &user_token)) {
        return false;  // nobody logged on in that session (yet)
    }
    UniqueHandle user(user_token);
    HANDLE primary_token = nullptr;
    if (!::DuplicateTokenEx(user.get(), MAXIMUM_ALLOWED, nullptr, SecurityIdentification, TokenPrimary,
                            &primary_token)) {
        log::error("DuplicateTokenEx failed for session ", session, ": ", last_error_message());
        return false;
    }
    UniqueHandle primary(primary_token);

    const auto exe = current_executable();
    if (!exe) {
        log::error("cannot determine the service executable path");
        return false;
    }

    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    UniqueHandle stop_event(::CreateEventW(&inheritable, TRUE, FALSE, nullptr));
    UniqueHandle reload_event(::CreateEventW(&inheritable, FALSE, FALSE, nullptr));
    if (!stop_event || !reload_event) {
        log::error("CreateEvent failed: ", last_error_message());
        return false;
    }

    // Only these two handles are inherited by the agent.
    HANDLE inherited[2] = {stop_event.get(), reload_event.get()};
    SIZE_T attribute_size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<std::byte> attribute_buffer(attribute_size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &attribute_size) ||
        !::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof inherited,
                                     nullptr, nullptr)) {
        log::error("cannot prepare agent process attributes: ", last_error_message());
        return false;
    }

    void* environment = nullptr;
    ::CreateEnvironmentBlock(&environment, primary.get(), FALSE);

    std::wstring command = L"\"" + exe->wstring() + L"\" windows-agent --stop-event " +
                           std::to_wstring(reinterpret_cast<std::uintptr_t>(stop_event.get())) + L" --reload-event " +
                           std::to_wstring(reinterpret_cast<std::uintptr_t>(reload_event.get()));
    wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof startup;
    startup.StartupInfo.lpDesktop = desktop;
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION info{};
    const BOOL created = ::CreateProcessAsUserW(
        primary.get(), exe->c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW, environment, nullptr,
        &startup.StartupInfo, &info);
    const DWORD error = ::GetLastError();
    if (environment != nullptr) {
        ::DestroyEnvironmentBlock(environment);
    }
    ::DeleteProcThreadAttributeList(attributes);
    if (!created) {
        log::error("cannot start the agent in session ", session, ": ", error_message(error));
        return false;
    }
    ::CloseHandle(info.hThread);
    agent.process.reset(info.hProcess);
    agent.stop_event = std::move(stop_event);
    agent.reload_event = std::move(reload_event);
    agent.started = Clock::now();
    agent.running = true;
    log::info("started agent (pid ", info.dwProcessId, ") in session ", session);
    return true;
}

void Supervisor::ensure_agent(DWORD session) {
    if (session == 0) {
        return;  // services session: no interactive desktop
    }
    AgentProcess& agent = agents_[session];
    if (agent.running || Clock::now() < agent.restart_at) {
        return;
    }
    if (!launch(session, agent)) {
        // Not an error if nobody is logged on there; forget the session in that case.
        if (::GetLastError() == ERROR_NO_TOKEN) {
            agents_.erase(session);
        } else {
            agent.restart_at = Clock::now() + agent.backoff;
            agent.backoff = std::min(agent.backoff * 2, kMaxRestartDelay);
        }
    }
}

void Supervisor::reap(DWORD session, AgentProcess& agent) {
    DWORD code = 0;
    ::GetExitCodeProcess(agent.process.get(), &code);
    agent.process.reset();
    agent.stop_event.reset();
    agent.reload_event.reset();
    agent.running = false;
    const auto ran_for = Clock::now() - agent.started;
    if (ran_for > kStableRun) {
        agent.backoff = kMinRestartDelay;
    }
    if (code == static_cast<DWORD>(kAgentExitOk)) {
        log::info("agent in session ", session, " exited");
    } else {
        log::warning("agent in session ", session, " exited with code ", code, "; restarting in ",
                     agent.backoff.count(), " s");
    }
    agent.restart_at = Clock::now() + agent.backoff;
    agent.backoff = std::min(agent.backoff * 2, kMaxRestartDelay);
}

void Supervisor::process_requests() {
    std::deque<std::pair<DWORD, DWORD>> events;
    bool reload = false;
    {
        std::lock_guard lock(mutex_);
        events.swap(session_events_);
        reload = std::exchange(reload_requested_, false);
    }
    for (const auto& [event, session] : events) {
        switch (event) {
            case WTS_SESSION_LOGON:
            case WTS_SESSION_UNLOCK:
            case WTS_CONSOLE_CONNECT:
            case WTS_REMOTE_CONNECT:
                agents_[session].restart_at = {};
                ensure_agent(session);
                break;
            case WTS_SESSION_LOGOFF: {
                // The agent ends with the session; do not restart it there.
                const auto it = agents_.find(session);
                if (it != agents_.end() && !it->second.running) {
                    agents_.erase(it);
                }
                break;
            }
            default:
                break;
        }
    }
    if (reload) {
        for (auto& [session, agent] : agents_) {
            if (agent.running) {
                ::SetEvent(agent.reload_event.get());
            }
        }
    }
}

void Supervisor::stop_all() {
    std::vector<HANDLE> processes;
    for (auto& [session, agent] : agents_) {
        if (agent.running) {
            ::SetEvent(agent.stop_event.get());  // graceful: the agent delivers held-back keys first
            processes.push_back(agent.process.get());
        }
    }
    if (!processes.empty()) {
        ::WaitForMultipleObjects(static_cast<DWORD>(processes.size()), processes.data(), TRUE, kAgentStopTimeoutMs);
        for (HANDLE process : processes) {
            if (::WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
                ::TerminateProcess(process, 1);
            }
        }
    }
    agents_.clear();
}

void Supervisor::run() {
    // Sessions that already exist (the service may start after users logged on).
    WTS_SESSION_INFOW* sessions = nullptr;
    DWORD count = 0;
    if (::WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &sessions, &count)) {
        for (DWORD i = 0; i < count; ++i) {
            if (sessions[i].State == WTSActive || sessions[i].State == WTSDisconnected ||
                sessions[i].State == WTSConnected) {
                ensure_agent(sessions[i].SessionId);
            }
        }
        ::WTSFreeMemory(sessions);
    }

    while (true) {
        std::vector<HANDLE> handles{stop_.get(), wake_.get()};
        std::vector<DWORD> owners;
        auto next_restart = Clock::time_point::max();
        for (auto& [session, agent] : agents_) {
            if (agent.running) {
                handles.push_back(agent.process.get());
                owners.push_back(session);
            } else {
                next_restart = std::min(next_restart, agent.restart_at);
            }
        }
        DWORD timeout = INFINITE;
        if (next_restart != Clock::time_point::max()) {
            const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next_restart - Clock::now());
            timeout = static_cast<DWORD>(std::clamp<long long>(wait.count(), 0, 60'000));
        }
        const DWORD result = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE,
                                                      timeout);
        if (result == WAIT_OBJECT_0) {
            break;
        }
        if (result == WAIT_OBJECT_0 + 1) {
            process_requests();
        } else if (result >= WAIT_OBJECT_0 + 2 && result < WAIT_OBJECT_0 + handles.size()) {
            const DWORD session = owners[result - WAIT_OBJECT_0 - 2];
            reap(session, agents_[session]);
        } else if (result == WAIT_FAILED) {
            log::error("WaitForMultipleObjects failed: ", last_error_message());
            ::Sleep(1000);
        }
        // Restart agents whose back-off elapsed, in sessions that still have a user.
        std::vector<DWORD> due;
        for (const auto& [session, agent] : agents_) {
            if (!agent.running && Clock::now() >= agent.restart_at) {
                due.push_back(session);
            }
        }
        for (const DWORD session : due) {
            ensure_agent(session);
        }
    }
    stop_all();
}

// ---------------------------------------------------------------------------------------------
// Service control manager plumbing
// ---------------------------------------------------------------------------------------------

SERVICE_STATUS_HANDLE g_status_handle = nullptr;
SERVICE_STATUS g_status{};
Supervisor* g_supervisor = nullptr;

void report_status(DWORD state, DWORD exit_code = NO_ERROR, DWORD wait_hint = 0) {
    static DWORD checkpoint = 1;
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwWin32ExitCode = exit_code;
    g_status.dwWaitHint = wait_hint;
    g_status.dwControlsAccepted =
        state == SERVICE_RUNNING ? SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN | SERVICE_ACCEPT_SESSIONCHANGE |
                                       SERVICE_ACCEPT_PARAMCHANGE
                                 : 0;
    g_status.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    ::SetServiceStatus(g_status_handle, &g_status);
}

DWORD WINAPI control_handler(DWORD control, DWORD event_type, LPVOID event_data, LPVOID) {
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            report_status(SERVICE_STOP_PENDING, NO_ERROR, kAgentStopTimeoutMs + 2000);
            if (g_supervisor != nullptr) {
                g_supervisor->request_stop();
            }
            return NO_ERROR;
        case SERVICE_CONTROL_SESSIONCHANGE:
            if (g_supervisor != nullptr && event_data != nullptr) {
                g_supervisor->session_changed(event_type,
                                              static_cast<const WTSSESSION_NOTIFICATION*>(event_data)->dwSessionId);
            }
            return NO_ERROR;
        case SERVICE_CONTROL_PARAMCHANGE:
            log::info("configuration reload requested");
            if (g_supervisor != nullptr) {
                g_supervisor->request_reload();
            }
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI service_main(DWORD, LPWSTR*) {
    g_status_handle = ::RegisterServiceCtrlHandlerExW(kServiceName, &control_handler, nullptr);
    if (g_status_handle == nullptr) {
        return;
    }
    report_status(SERVICE_START_PENDING, NO_ERROR, 3000);

    const Paths p = paths();
    std::error_code ec;
    std::filesystem::create_directories(p.service_log.parent_path(), ec);
    if (!log::logger().use_file(p.service_log)) {
        log::logger().use_stderr();
    }
    log::info(build::kProgramName, ' ', build::kVersion, " service starting");

    try {
        Supervisor supervisor;
        g_supervisor = &supervisor;
        report_status(SERVICE_RUNNING);
        supervisor.run();
        g_supervisor = nullptr;
    } catch (const std::exception& e) {
        g_supervisor = nullptr;
        log::error("service failed: ", e.what());
        report_status(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }
    log::info("service stopped");
    report_status(SERVICE_STOPPED);
}

}  // namespace

int run_service() {
    wchar_t name[] = L"keyboard-chatter-filter";
    SERVICE_TABLE_ENTRYW table[] = {{name, &service_main}, {nullptr, nullptr}};
    if (!::StartServiceCtrlDispatcherW(table)) {
        if (::GetLastError() == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
            log::error("'windows-service' is started by the Windows service manager; use "
                       "'keyboard-chatter-filter start' instead");
        } else {
            log::error("StartServiceCtrlDispatcher failed: ", last_error_message());
        }
        return 1;
    }
    return 0;
}

}  // namespace kcf::windows
