// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>

#include <array>
#include <bitset>
#include <cstdint>
#include <vector>

#include "keyboard_filter/keyboard_io.h"
#include "windows/key_mapping.h"

namespace kcf::windows {

// Value placed in KEYBDINPUT::dwExtraInfo of every event we inject, so the hook recognises its own
// re-injections (they also carry LLKHF_INJECTED, which the hook never filters anyway).
inline constexpr ULONG_PTR kInjectedEventMarker = 0x4B43465FUL;  // "KCF_"

// Delivers held-back key events with SendInput.
//
// A low-level hook cannot insert an event ahead of the one it is processing. When the filter has to
// deliver a held-back *modifier* release before the current event (Shift released 5 ms before the
// next letter), the hook blocks the current event and both are re-injected in the right order.
// Held-back releases of ordinary keys are injected right after the current event instead: the
// order of two non-modifier events never changes what applications do with them.
//
// SendInput is never called from inside the hook procedure; queued input is sent by the agent's
// message loop immediately after the hook returns.
class WinKeyboardOutput final : public IKeyboardOutput {
public:
    WinKeyboardOutput();

    void emit(const KeyEvent& event) override;

    // Remembers the native fields of an event the filter deferred.
    void stash(KeyCode code, const HookEvent& event) noexcept;

    // Bracket a hook callback. end_hook() returns true if the current event must be blocked and
    // re-injected after the releases emitted during the callback.
    void begin_hook() noexcept;
    [[nodiscard]] bool end_hook();
    void queue_reinjection(const HookEvent& event, bool key_up);

    // Sends everything queued. Called from the message loop, outside the hook.
    void send_queued();
    [[nodiscard]] bool has_queued() const noexcept { return !queue_.empty(); }
    [[nodiscard]] std::uint64_t send_failures() const noexcept { return send_failures_; }

private:
    [[nodiscard]] INPUT make_input(const HookEvent& event, bool key_up) const noexcept;

    std::vector<HookEvent> stash_;
    std::bitset<kKeyCodeLimit> stashed_;
    std::vector<INPUT> during_hook_;
    std::vector<INPUT> queue_;
    bool in_hook_ = false;
    bool modifier_flushed_ = false;
    std::uint64_t send_failures_ = 0;
};

}  // namespace kcf::windows
