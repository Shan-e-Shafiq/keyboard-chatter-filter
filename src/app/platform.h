// SPDX-License-Identifier: MIT
//
// Operations every platform implements (src/<os>/platform.cpp). main() parses the command line and
// dispatches here; nothing in this interface is OS-specific.
#pragma once

#include <filesystem>
#include <optional>

#include "common/cli.h"

namespace kcf::platform {

// Where the configuration file lives when --config is not given.
[[nodiscard]] std::filesystem::path default_config_path();

// Runs the filter in the foreground until stopped. Returns the process exit code.
int run(const CliOptions& options);

int install(const CliOptions& options);
int uninstall(const CliOptions& options);
int start();
int stop();
int restart();
int reload();
int status();

// Platform-specific commands (Windows service/agent entry points). Returns std::nullopt if the
// command does not exist on this platform.
std::optional<int> run_platform_command(const CliOptions& options);

}  // namespace kcf::platform
