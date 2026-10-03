// SPDX-License-Identifier: MIT
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include "app/platform.h"
#include "common/cli.h"
#include "common/logging.h"
#include "kcf/build_info.h"
#include "keyboard_filter/configuration.h"

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

namespace {

int show_config(const kcf::CliOptions& options) {
    const std::filesystem::path path = options.config_path.value_or(kcf::platform::default_config_path());
    const kcf::ConfigLoadResult result = kcf::load_configuration(path);
    std::cout << "Configuration file: " << path.string() << (result.file_found ? "" : " (not present; defaults apply)")
              << "\n\n";
    std::cout << "  chatter_threshold_ms = " << result.config.chatter_threshold.count() << '\n'
              << "  enabled = " << (result.config.enabled ? "true" : "false") << '\n'
              << "  log_level = \"" << kcf::to_string(result.config.log_level) << "\"\n";
    if (!result.config.ignored_devices.empty()) {
        std::cout << "  ignored_devices = [";
        for (std::size_t i = 0; i < result.config.ignored_devices.size(); ++i) {
            std::cout << (i ? ", " : "") << '"' << result.config.ignored_devices[i] << '"';
        }
        std::cout << "]\n";
    }
    const auto problems = result.describe(path);
    if (problems.empty()) {
        std::cout << "\nNo problems found.\n";
        return 0;
    }
    std::cout << '\n';
    for (const std::string& line : problems) {
        std::cout << line << '\n';
    }
    return result.has_errors() ? 1 : 0;
}

int dispatch(const kcf::CliOptions& options) {
    using kcf::Command;
    switch (options.command) {
        case Command::Run: return kcf::platform::run(options);
        case Command::Start: return kcf::platform::start();
        case Command::Stop: return kcf::platform::stop();
        case Command::Restart: return kcf::platform::restart();
        case Command::Reload: return kcf::platform::reload();
        case Command::Status: return kcf::platform::status();
        case Command::Install: return kcf::platform::install(options);
        case Command::Uninstall: return kcf::platform::uninstall(options);
        case Command::Config: return show_config(options);
        case Command::Version:
            std::cout << kcf::build::kProgramName << ' ' << kcf::build::kVersion << '\n';
            return 0;
        case Command::Help:
            std::cout << kcf::usage_text();
            return 0;
        case Command::WindowsService:
        case Command::WindowsAgent:
            if (const auto code = kcf::platform::run_platform_command(options)) {
                return *code;
            }
            std::cerr << "error: this command is only available on Windows\n";
            return 2;
    }
    return 2;
}

int real_main(const std::vector<std::string>& arguments) {
    std::vector<std::string_view> args(arguments.begin(), arguments.end());
    const kcf::CliParseResult parsed = kcf::parse_command_line(args);
    if (!parsed.options) {
        std::cerr << "error: " << parsed.error << "\n\nRun 'keyboard-chatter-filter help' for usage.\n";
        return 2;
    }
    kcf::log::logger().use_stderr();
    return dispatch(*parsed.options);
}

}  // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const int size = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string arg(size > 0 ? static_cast<std::size_t>(size - 1) : 0, '\0');
        if (size > 1) {
            WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, arg.data(), size, nullptr, nullptr);
        }
        arguments.push_back(std::move(arg));
    }
#else
int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + (argc > 0 ? 1 : 0), argv + argc);
#endif
    try {
        return real_main(arguments);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "fatal: %s\n", e.what());
    } catch (...) {
        std::fprintf(stderr, "fatal: unexpected error\n");
    }
    return 1;
}
