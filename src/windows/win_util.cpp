// SPDX-License-Identifier: MIT
#include "windows/win_util.h"

#include <knownfolders.h>
#include <sddl.h>
#include <shlobj.h>

#include <vector>

#include "kcf/build_info.h"

namespace kcf::windows {

std::wstring widen(std::string_view utf8) {
    if (utf8.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), size);
    }
    return out;
}

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) {
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size, nullptr,
                              nullptr);
    }
    return out;
}

std::string error_message(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD length = ::FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, error, 0,
        reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::string text = length > 0 ? narrow(std::wstring_view(buffer, length)) : std::string("unknown error");
    if (buffer != nullptr) {
        ::LocalFree(buffer);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '.')) {
        text.pop_back();
    }
    return text + " (code " + std::to_string(error) + ")";
}

std::string last_error_message() {
    return error_message(::GetLastError());
}

std::int64_t qpc_nanoseconds() noexcept {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER f{};
        ::QueryPerformanceFrequency(&f);
        return f.QuadPart > 0 ? f.QuadPart : 1;
    }();
    LARGE_INTEGER counter{};
    ::QueryPerformanceCounter(&counter);
    const std::int64_t whole = counter.QuadPart / frequency;
    const std::int64_t rest = counter.QuadPart % frequency;
    return whole * 1'000'000'000 + rest * 1'000'000'000 / frequency;
}

namespace {

std::filesystem::path known_folder(REFKNOWNFOLDERID id, const wchar_t* fallback_env) {
    PWSTR path = nullptr;
    std::filesystem::path result;
    if (SUCCEEDED(::SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) && path != nullptr) {
        result = path;
    } else {
        wchar_t buffer[MAX_PATH] = {};
        if (::GetEnvironmentVariableW(fallback_env, buffer, MAX_PATH) > 0) {
            result = buffer;
        }
    }
    ::CoTaskMemFree(path);
    return result;
}

}  // namespace

Paths paths() {
    const std::wstring name = widen(build::kProgramName);
    Paths p;
    p.program_data_dir = known_folder(FOLDERID_ProgramData, L"ProgramData") / name;
    p.config_file = p.program_data_dir / L"config.toml";
    p.service_log = p.program_data_dir / L"logs" / L"service.log";
    p.user_log = known_folder(FOLDERID_LocalAppData, L"LOCALAPPDATA") / name / L"agent.log";
    p.program_files_dir = known_folder(FOLDERID_ProgramFiles, L"ProgramFiles") / name;
    return p;
}

std::optional<std::filesystem::path> current_executable() {
    std::vector<wchar_t> buffer(MAX_PATH);
    while (true) {
        const DWORD n = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (n == 0) {
            return std::nullopt;
        }
        if (n < buffer.size()) {
            return std::filesystem::path(std::wstring(buffer.data(), n));
        }
        buffer.resize(buffer.size() * 2);
    }
}

bool create_protected_directory(const std::filesystem::path& dir, std::string& error) {
    std::error_code ec;
    if (dir.has_parent_path()) {
        std::filesystem::create_directories(dir.parent_path(), ec);
    }
    // P: protected (no inheritance from %ProgramData%, which lets every user create files).
    // SYSTEM and Administrators: full control. Users: read and execute.
    const wchar_t* sddl = L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl, SDDL_REVISION_1, &descriptor, nullptr)) {
        error = "cannot build security descriptor: " + last_error_message();
        return false;
    }
    bool ok = true;
    if (!::CreateDirectoryW(dir.c_str(), nullptr) && ::GetLastError() != ERROR_ALREADY_EXISTS) {
        error = "cannot create " + narrow(dir.wstring()) + ": " + last_error_message();
        ok = false;
    } else if (!::SetFileSecurityW(dir.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                   descriptor)) {
        error = "cannot secure " + narrow(dir.wstring()) + ": " + last_error_message();
        ok = false;
    }
    ::LocalFree(descriptor);
    return ok;
}

bool is_elevated() {
    HANDLE token = nullptr;
    if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool elevated =
        ::GetTokenInformation(token, TokenElevation, &elevation, sizeof elevation, &size) && elevation.TokenIsElevated;
    ::CloseHandle(token);
    return elevated;
}

}  // namespace kcf::windows
