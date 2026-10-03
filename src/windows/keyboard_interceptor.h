// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>

#include <cstdint>

#include "keyboard_filter/keyboard_io.h"
#include "windows/keyboard_output.h"

namespace kcf::windows {

class QpcClock final : public IClock {
public:
    [[nodiscard]] Timestamp now() const noexcept override;
};

// System-wide keyboard interception with a low-level keyboard hook (WH_KEYBOARD_LL). Runs on the
// thread that installs it, which must pump messages. Only events from physical keyboards are
// filtered (anything with LLKHF_INJECTED passes untouched).
//
// The hook is the replaceable part of the Windows adapter: a kernel filter driver could implement
// the same IKeyboardInterceptor/IKeyboardOutput pair without any change to the filter.
class WinKeyboardInterceptor final : public IKeyboardInterceptor {
public:
    WinKeyboardInterceptor(WinKeyboardOutput& output, const QpcClock& clock);
    ~WinKeyboardInterceptor() override;
    WinKeyboardInterceptor(const WinKeyboardInterceptor&) = delete;
    WinKeyboardInterceptor& operator=(const WinKeyboardInterceptor&) = delete;

    InterceptorStartResult start(IKeyEventHandler& handler) override;
    void stop() noexcept override;
    [[nodiscard]] bool is_active() const noexcept override { return hook_ != nullptr; }

    // Dry run: events are classified (and counted) but never blocked.
    void set_dry_run(bool dry_run) noexcept { dry_run_ = dry_run; }

private:
    static LRESULT CALLBACK hook_proc(int code, WPARAM wparam, LPARAM lparam);
    bool handle(const KBDLLHOOKSTRUCT& event);  // true = block the event

    WinKeyboardOutput& output_;
    const QpcClock& clock_;
    IKeyEventHandler* handler_ = nullptr;
    HHOOK hook_ = nullptr;
    ModifierState modifiers_;
    bool dry_run_ = false;
};

}  // namespace kcf::windows
