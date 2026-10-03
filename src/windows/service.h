// SPDX-License-Identifier: MIT
#pragma once

namespace kcf::windows {

// Entry point when started by the service control manager (`keyboard-chatter-filter
// windows-service`). The service itself never touches keyboard input: it keeps one agent process
// running in every interactive session and restarts agents that fail.
int run_service();

}  // namespace kcf::windows
