// SPDX-License-Identifier: MIT
//
// Windows implementation of the CLI commands. A Windows service (LocalSystem, automatic start,
// restart on failure) keeps a per-session agent running; these commands manage that service
// through the Service Control Manager API.
#include "app/platform.h"

#include <chrono>
#include <iostream>
#include <thread>

#include "common/logging.h"
#include "kcf/build_info.h"
#include "windows/agent.h"
#include "windows/service.h"
#include "windows/win_util.h"

namespace kcf::windows {
namespace {

bool require_elevation(const char* what) {
    if (is_elevated()) {
        return true;
    }
    std::cerr << "error: '" << what << "' needs administrator rights. Run it from an elevated terminal "
              << "(right-click Terminal > Run as administrator).\n";
    return false;
}

ServiceHandle open_manager(DWORD access) {
    return ServiceHandle(::OpenSCManagerW(nullptr, nullptr, access));
}

ServiceHandle open_service(DWORD access) {
    const ServiceHandle manager = open_manager(SC_MANAGER_CONNECT);
    if (!manager) {
        return ServiceHandle();
    }
    return ServiceHandle(::OpenServiceW(manager.get(), kServiceName, access));
}

std::optional<SERVICE_STATUS_PROCESS> query(const ServiceHandle& service) {
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (!::QueryServiceStatusEx(service.get(), SC_STATUS_PROCESS_INFO, reinterpret_cast<LPBYTE>(&status),
                                sizeof status, &needed)) {
        return std::nullopt;
    }
    return status;
}

bool wait_for_state(const ServiceHandle& service, DWORD wanted, std::chrono::seconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto status = query(service);
        if (!status) {
            return false;
        }
        if (status->dwCurrentState == wanted) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return false;
}

const char* state_name(DWORD state) {
    switch (state) {
        case SERVICE_STOPPED: return "stopped";
        case SERVICE_START_PENDING: return "starting";
        case SERVICE_STOP_PENDING: return "stopping";
        case SERVICE_RUNNING: return "running";
        case SERVICE_CONTINUE_PENDING: return "continuing";
        case SERVICE_PAUSE_PENDING: return "pausing";
        case SERVICE_PAUSED: return "paused";
        default: return "unknown";
    }
}

bool agent_running_in_this_session() {
    UniqueHandle mutex(::OpenMutexW(SYNCHRONIZE, FALSE, kAgentMutexName));
    return mutex.valid();
}

// The service runs as LocalSystem, so its executable must not be replaceable by ordinary users:
// only accept binaries inside %ProgramFiles%.
bool is_in_program_files(const std::filesystem::path& exe) {
    const std::wstring base = paths().program_files_dir.parent_path().wstring();
    const std::wstring path = exe.wstring();
    return !base.empty() && path.size() > base.size() &&
           ::CompareStringOrdinal(path.c_str(), static_cast<int>(base.size()), base.c_str(),
                                  static_cast<int>(base.size()), TRUE) == CSTR_EQUAL &&
           (path[base.size()] == L'\\' || path[base.size()] == L'/');
}

int start_service_and_report() {
    const ServiceHandle service = open_service(SERVICE_START | SERVICE_QUERY_STATUS);
    if (!service) {
        std::cerr << "error: the service is not installed (" << last_error_message() << ")\n";
        return 1;
    }
    if (!::StartServiceW(service.get(), 0, nullptr) && ::GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
        std::cerr << "error: cannot start the service: " << last_error_message() << '\n';
        return 1;
    }
    if (!wait_for_state(service, SERVICE_RUNNING, std::chrono::seconds(10))) {
        std::cerr << "error: the service did not start; see " << narrow(paths().service_log.wstring()) << '\n';
        return 1;
    }
    for (int i = 0; i < 25 && !agent_running_in_this_session(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    std::cout << "keyboard-chatter-filter is running"
              << (agent_running_in_this_session() ? " and filtering this session's keyboard input.\n" : ".\n");
    return 0;
}

int stop_service() {
    const ServiceHandle service = open_service(SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (!service) {
        const DWORD error = ::GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) {
            return 0;
        }
        std::cerr << "error: cannot open the service: " << error_message(error) << '\n';
        return 1;
    }
    SERVICE_STATUS status{};
    if (!::ControlService(service.get(), SERVICE_CONTROL_STOP, &status) &&
        ::GetLastError() != ERROR_SERVICE_NOT_ACTIVE) {
        std::cerr << "error: cannot stop the service: " << last_error_message() << '\n';
        return 1;
    }
    if (!wait_for_state(service, SERVICE_STOPPED, std::chrono::seconds(15))) {
        std::cerr << "error: the service did not stop in time\n";
        return 1;
    }
    return 0;
}

}  // namespace
}  // namespace kcf::windows

namespace kcf::platform {

using namespace kcf::windows;

std::filesystem::path default_config_path() {
    return paths().config_file;
}

int run(const CliOptions& options) {
    AgentOptions agent;
    agent.config_path = options.config_path.value_or(default_config_path());
    agent.log_level_override = options.log_level;
    return run_agent(agent);
}

int install(const CliOptions&) {
    if (!require_elevation("install")) {
        return 1;
    }
    const auto exe = current_executable();
    if (!exe) {
        std::cerr << "error: cannot determine the path of this executable\n";
        return 1;
    }
    if (!is_in_program_files(*exe)) {
        std::cerr << "error: refusing to register " << narrow(exe->wstring())
                  << " as a service: the service runs as LocalSystem, so its executable must live in a folder "
                     "only administrators can modify. Copy it to "
                  << narrow(paths().program_files_dir.wstring()) << " first (install.ps1 does this).\n";
        return 1;
    }

    const Paths p = paths();
    std::string error;
    if (!create_protected_directory(p.program_data_dir, error)) {
        std::cerr << "error: " << error << '\n';
        return 1;
    }
    std::error_code ec;
    std::filesystem::create_directories(p.service_log.parent_path(), ec);
    if (!std::filesystem::exists(p.config_file)) {
        std::string text = default_configuration_text();
        // Windows editors handle LF fine; keep the file identical on all platforms.
        if (FILE* f = _wfopen(p.config_file.c_str(), L"wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
            std::cout << "Created configuration file " << narrow(p.config_file.wstring()) << '\n';
        }
    }

    const ServiceHandle manager = open_manager(SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!manager) {
        std::cerr << "error: cannot open the service manager: " << last_error_message() << '\n';
        return 1;
    }
    const std::wstring command = L"\"" + exe->wstring() + L"\" windows-service";
    ServiceHandle service(::CreateServiceW(manager.get(), kServiceName, kServiceDisplayName, SERVICE_ALL_ACCESS,
                                           SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                           command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!service && ::GetLastError() == ERROR_SERVICE_EXISTS) {
        // Reinstall/upgrade: stop the old instance and point the service at this binary.
        stop_service();
        service = ServiceHandle(::OpenServiceW(manager.get(), kServiceName, SERVICE_ALL_ACCESS));
        if (service && !::ChangeServiceConfigW(service.get(), SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START,
                                               SERVICE_ERROR_NORMAL, command.c_str(), nullptr, nullptr, nullptr,
                                               nullptr, nullptr, kServiceDisplayName)) {
            std::cerr << "error: cannot update the service: " << last_error_message() << '\n';
            return 1;
        }
    }
    if (!service) {
        std::cerr << "error: cannot create the service: " << last_error_message() << '\n';
        return 1;
    }

    std::wstring description =
        L"Removes duplicate keystrokes caused by keyboard switch chatter. Keystrokes are processed in memory only; "
        L"nothing is logged or transmitted.";
    SERVICE_DESCRIPTIONW desc{description.data()};
    ::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_DESCRIPTION, &desc);

    // Restart after 5 s on the first three failures within a day.
    SC_ACTION actions[3] = {{SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 5000}, {SC_ACTION_RESTART, 30000}};
    SERVICE_FAILURE_ACTIONSW failure{};
    failure.dwResetPeriod = 24 * 60 * 60;
    failure.cActions = 3;
    failure.lpsaActions = actions;
    if (!::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS, &failure)) {
        std::cerr << "warning: cannot configure automatic restart: " << last_error_message() << '\n';
    }
    SERVICE_FAILURE_ACTIONS_FLAG flag{TRUE};  // also when the service stops with an error code
    ::ChangeServiceConfig2W(service.get(), SERVICE_CONFIG_FAILURE_ACTIONS_FLAG, &flag);

    std::cout << "Installed the '" << narrow(kServiceName) << "' service (starts automatically)\n";
    return start_service_and_report();
}

int start() {
    return start_service_and_report();
}

int stop() {
    const int rc = stop_service();
    if (rc == 0) {
        std::cout << "Stopped. The keyboard is unfiltered until the next restart or 'keyboard-chatter-filter "
                     "start'.\n";
    }
    return rc;
}

int restart() {
    const int rc = stop_service();
    return rc != 0 ? rc : start_service_and_report();
}

int reload() {
    const ServiceHandle service = open_service(SERVICE_PAUSE_CONTINUE);
    if (!service) {
        std::cerr << "error: cannot open the service: " << last_error_message()
                  << (is_elevated() ? "" : " (run from an elevated terminal)") << '\n';
        return 1;
    }
    SERVICE_STATUS status{};
    if (!::ControlService(service.get(), SERVICE_CONTROL_PARAMCHANGE, &status)) {
        std::cerr << "error: cannot reach the service: " << last_error_message() << '\n';
        return 1;
    }
    std::cout << "Configuration reload requested. Problems, if any, are logged in "
              << narrow(paths().user_log.wstring()) << '\n';
    return 0;
}

int status() {
    const Paths p = paths();
    const ServiceHandle service = open_service(SERVICE_QUERY_STATUS);
    const bool agent = agent_running_in_this_session();
    if (!service) {
        if (agent) {
            std::cout << "keyboard-chatter-filter is running in the foreground in this session (service not "
                         "installed).\n";
            return 0;
        }
        std::cout << "keyboard-chatter-filter is not installed.\n";
        return 3;
    }
    const auto state = query(service);
    const DWORD current = state ? state->dwCurrentState : 0;
    std::cout << "keyboard-chatter-filter service: " << state_name(current) << '\n'
              << "  Filtering this session: " << (agent ? "yes" : "no") << '\n'
              << "  Config:      " << narrow(p.config_file.wstring()) << '\n'
              << "  Service log: " << narrow(p.service_log.wstring()) << '\n'
              << "  Agent log:   " << narrow(p.user_log.wstring()) << '\n';
    if (current != SERVICE_RUNNING) {
        return 3;
    }
    return agent ? 0 : 1;
}

int uninstall(const CliOptions& options) {
    if (!require_elevation("uninstall")) {
        return 1;
    }
    int rc = stop_service();
    const ServiceHandle service = open_service(DELETE);
    if (service) {
        if (::DeleteService(service.get())) {
            std::cout << "Removed the '" << narrow(kServiceName) << "' service\n";
        } else if (::GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE) {
            std::cerr << "error: cannot remove the service: " << last_error_message() << '\n';
            rc = 1;
        }
    }

    const Paths p = paths();
    std::error_code ec;
    if (options.purge) {
        for (const auto& file : {p.config_file, p.service_log, std::filesystem::path(p.service_log.wstring() + L".1"),
                                 p.user_log, std::filesystem::path(p.user_log.wstring() + L".1")}) {
            if (std::filesystem::remove(file, ec)) {
                std::cout << "Removed " << narrow(file.wstring()) << '\n';
            }
        }
        std::filesystem::remove(p.service_log.parent_path(), ec);  // only if empty
        std::filesystem::remove(p.program_data_dir, ec);
        std::filesystem::remove(p.user_log.parent_path(), ec);
    } else if (std::filesystem::exists(p.config_file, ec)) {
        std::cout << "Kept " << narrow(p.config_file.wstring()) << " (use --purge to remove it)\n";
    }

    // A running executable cannot delete itself on Windows: schedule removal of the installed copy.
    const auto exe = current_executable();
    const auto installed = p.program_files_dir / (std::wstring(widen(build::kProgramName)) + L".exe");
    if (std::filesystem::exists(installed, ec)) {
        if (exe && std::filesystem::equivalent(*exe, installed, ec)) {
            if (::MoveFileExW(installed.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
                ::MoveFileExW(p.program_files_dir.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                std::cout << narrow(installed.wstring()) << " will be removed at the next restart "
                          << "(uninstall.ps1 removes it immediately)\n";
            }
        } else if (std::filesystem::remove(installed, ec)) {
            std::filesystem::remove(p.program_files_dir, ec);
            std::cout << "Removed " << narrow(installed.wstring()) << '\n';
        }
    }
    std::cout << "keyboard-chatter-filter has been uninstalled.\n";
    return rc;
}

std::optional<int> run_platform_command(const CliOptions& options) {
    if (options.command == Command::WindowsService) {
        return run_service();
    }
    if (options.command == Command::WindowsAgent) {
        AgentOptions agent;
        agent.config_path = options.config_path.value_or(default_config_path());
        agent.log_level_override = options.log_level;
        agent.stop_event = options.stop_event ? reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(*options.stop_event))
                                              : nullptr;
        agent.reload_event =
            options.reload_event ? reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(*options.reload_event)) : nullptr;
        // Launched by the service: log to the user's profile.
        const auto log_file = paths().user_log;
        std::error_code ec;
        std::filesystem::create_directories(log_file.parent_path(), ec);
        if (!log::logger().use_file(log_file)) {
            log::logger().use_stderr();
        }
        return run_agent(agent);
    }
    return std::nullopt;
}

}  // namespace kcf::platform
