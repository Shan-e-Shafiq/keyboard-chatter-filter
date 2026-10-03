// SPDX-License-Identifier: MIT
#include "windows/keyboard_interceptor.h"

#include <exception>

#include "common/logging.h"
#include "windows/key_mapping.h"
#include "windows/win_util.h"

namespace kcf::windows {

static_assert(llkhf::kExtended == LLKHF_EXTENDED && llkhf::kInjected == LLKHF_INJECTED);
static_assert(llkhf::kLowerIlInjected == LLKHF_LOWER_IL_INJECTED && llkhf::kUp == LLKHF_UP);
static_assert(llkhf::kAltDown == LLKHF_ALTDOWN);
static_assert(vk::kLShift == VK_LSHIFT && vk::kRControl == VK_RCONTROL && vk::kRMenu == VK_RMENU);
static_assert(vk::kLWin == VK_LWIN && vk::kPacket == VK_PACKET && vk::kCapital == VK_CAPITAL);
static_assert(keyeventf::kExtendedKey == KEYEVENTF_EXTENDEDKEY && keyeventf::kKeyUp == KEYEVENTF_KEYUP);

namespace {
// The hook procedure has no context parameter; the single active interceptor is reachable here.
// Only touched on the hook thread.
WinKeyboardInterceptor* g_active = nullptr;
}  // namespace

Timestamp QpcClock::now() const noexcept {
    return Timestamp(qpc_nanoseconds());
}

WinKeyboardInterceptor::WinKeyboardInterceptor(WinKeyboardOutput& output, const QpcClock& clock)
    : output_(output), clock_(clock) {}

WinKeyboardInterceptor::~WinKeyboardInterceptor() {
    stop();
}

InterceptorStartResult WinKeyboardInterceptor::start(IKeyEventHandler& handler) {
    if (hook_ != nullptr) {
        return {};
    }
    if (g_active != nullptr) {
        return {InterceptorError::Unavailable, "another keyboard hook is already active in this process"};
    }
    handler_ = &handler;
    g_active = this;
    hook_ = ::SetWindowsHookExW(WH_KEYBOARD_LL, &WinKeyboardInterceptor::hook_proc, ::GetModuleHandleW(nullptr), 0);
    if (hook_ == nullptr) {
        g_active = nullptr;
        handler_ = nullptr;
        return {InterceptorError::Unavailable, "SetWindowsHookEx(WH_KEYBOARD_LL) failed: " + last_error_message()};
    }
    return {};
}

void WinKeyboardInterceptor::stop() noexcept {
    if (hook_ != nullptr) {
        ::UnhookWindowsHookEx(hook_);
        hook_ = nullptr;
    }
    if (g_active == this) {
        g_active = nullptr;
    }
    handler_ = nullptr;
}

LRESULT CALLBACK WinKeyboardInterceptor::hook_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && g_active != nullptr && lparam != 0) {
        try {
            if (g_active->handle(*reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam))) {
                return 1;  // swallow
            }
        } catch (const std::exception& e) {
            log::error("unexpected error while handling a keyboard event: ", e.what());
        } catch (...) {
            log::error("unexpected error while handling a keyboard event");
        }
    }
    (void)wparam;
    return ::CallNextHookEx(nullptr, code, wparam, lparam);
}

bool WinKeyboardInterceptor::handle(const KBDLLHOOKSTRUCT& raw) {
    if (handler_ == nullptr || (raw.flags & LLKHF_INJECTED) != 0) {
        return false;  // software input, including our own re-injections
    }
    const HookEvent event{raw.vkCode, raw.scanCode, raw.flags};
    const bool down = (raw.flags & LLKHF_UP) == 0;
    update_modifiers(modifiers_, raw.vkCode, down);
    const std::optional<KeyEvent> key = to_key_event(event, clock_.now(), modifiers_);
    if (!key) {
        return false;
    }

    output_.begin_hook();
    const Decision decision = handler_->on_key_event(*key);
    const bool reinject_current = output_.end_hook();

    switch (decision) {
        case Decision::Accept:
            if (reinject_current) {
                output_.queue_reinjection(event, !down);
                return true;
            }
            return false;
        case Decision::Defer:
            output_.stash(key->code, event);
            return true;
        case Decision::Reject:
            return true;
    }
    return false;
}

}  // namespace kcf::windows
