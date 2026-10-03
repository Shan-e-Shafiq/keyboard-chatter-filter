// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace kcf::windows {

// Owning kernel HANDLE (CloseHandle). Treats both nullptr and INVALID_HANDLE_VALUE as empty.
class UniqueHandle {
public:
    UniqueHandle() noexcept = default;
    explicit UniqueHandle(HANDLE h) noexcept : handle_(h) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.handle_, nullptr));
        }
        return *this;
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE; }
    explicit operator bool() const noexcept { return valid(); }
    void reset(HANDLE h = nullptr) noexcept {
        if (valid()) {
            ::CloseHandle(handle_);
        }
        handle_ = h;
    }
    HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

private:
    HANDLE handle_ = nullptr;
};

// Owning service-control-manager handle (CloseServiceHandle).
class ServiceHandle {
public:
    ServiceHandle() noexcept = default;
    explicit ServiceHandle(SC_HANDLE h) noexcept : handle_(h) {}
    ~ServiceHandle() {
        if (handle_ != nullptr) {
            ::CloseServiceHandle(handle_);
        }
    }
    ServiceHandle(ServiceHandle&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
    ServiceHandle& operator=(ServiceHandle&& other) noexcept {
        if (this != &other) {
            if (handle_ != nullptr) {
                ::CloseServiceHandle(handle_);
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }
    ServiceHandle(const ServiceHandle&) = delete;
    ServiceHandle& operator=(const ServiceHandle&) = delete;

    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    SC_HANDLE handle_ = nullptr;
};

[[nodiscard]] std::wstring widen(std::string_view utf8);
[[nodiscard]] std::string narrow(std::wstring_view wide);

// "message (code N)" for a Win32 error code.
[[nodiscard]] std::string error_message(DWORD error);
[[nodiscard]] std::string last_error_message();

// Monotonic nanoseconds from QueryPerformanceCounter.
[[nodiscard]] std::int64_t qpc_nanoseconds() noexcept;

struct Paths {
    std::filesystem::path program_data_dir;  // %ProgramData%\keyboard-chatter-filter (config + service log)
    std::filesystem::path config_file;
    std::filesystem::path service_log;
    std::filesystem::path user_log;          // %LOCALAPPDATA%\keyboard-chatter-filter\agent.log
    std::filesystem::path program_files_dir; // %ProgramFiles%\keyboard-chatter-filter
};

[[nodiscard]] Paths paths();

[[nodiscard]] std::optional<std::filesystem::path> current_executable();

// Creates the directory (and parents) with an explicit ACL: SYSTEM and Administrators full control,
// Users read-only. Used for machine-wide configuration and the service log.
bool create_protected_directory(const std::filesystem::path& dir, std::string& error);

[[nodiscard]] bool is_elevated();

inline constexpr const wchar_t* kServiceName = L"keyboard-chatter-filter";
inline constexpr const wchar_t* kServiceDisplayName = L"Keyboard Chatter Filter";
inline constexpr const wchar_t* kAgentMutexName = L"Local\\keyboard-chatter-filter-agent";

}  // namespace kcf::windows
