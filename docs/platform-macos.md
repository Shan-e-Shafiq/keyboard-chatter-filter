# macOS

Supported: macOS 11 Big Sur and later, Apple silicon (arm64) and Intel (x86_64). Built-in, USB and
Bluetooth keyboards are all handled the same way: the filter works on the keyboard events macOS
produces, not on any particular transport.

## How it runs

* `keyboard-chatter-filter install` writes `~/Library/LaunchAgents/keyboard-chatter-filter.plist`
  and loads it with `launchctl bootstrap gui/<uid>`. From then on launchd starts the filter at every
  login. Nothing runs as root.
* The agent definition (`packaging/macos/launch-agent.plist.in`) uses:
  * `RunAtLoad` — start at login;
  * `KeepAlive` → `SuccessfulExit = false` — launchd restarts the filter if it crashes or exits with
    an error, but not after a deliberate `stop`;
  * `ThrottleInterval = 10` — at most one restart every 10 s;
  * `ProcessType = Interactive` — keyboard latency must not be throttled like background work;
  * `LimitLoadToSessionType = Aqua` — only in graphical login sessions;
  * `Umask = 077` and a pre-created log file with mode 0600;
  * an absolute `ProgramArguments` path to the installed binary.
* Logs: `~/Library/Logs/keyboard-chatter-filter/keyboard-chatter-filter.log` (lifecycle and errors
  only; never keystrokes). The file is truncated when it exceeds 4 MiB.
* Configuration: `~/.config/keyboard-chatter-filter/config.toml`.
* Runtime state (lock, status socket): `~/Library/Application Support/keyboard-chatter-filter/`.

## Permissions

Filtering keyboard input means dropping events system-wide, which macOS only allows to processes
the user has explicitly trusted under **Accessibility**.

| What | Where | Why |
|---|---|---|
| **Accessibility** (required) | System Settings → Privacy & Security → Accessibility | An *active* event tap (`kCGEventTapOptionDefault`, able to drop events) and posting the held-back key-up events require it. |
| Input Monitoring | not requested | Only needed for *listen-only* taps. |
| Root / administrator | not needed | Everything runs as the logged-in user. |

When launchd starts the filter and the permission is missing, the filter asks macOS to show its
standard prompt once (`AXIsProcessTrustedWithOptions`), which also adds
`keyboard-chatter-filter` to the Accessibility list. **No program can grant this permission to
itself**; you have to switch it on. Until then the keyboard simply works unfiltered, and the filter
checks again every 2 seconds and starts filtering as soon as access is granted, with no restart
needed. `keyboard-chatter-filter status` shows `waiting-for-permission` in the meantime.

If the entry is not in the list, click **+**, press **Cmd+Shift+G** and enter the binary's path
(`~/.local/bin/keyboard-chatter-filter` with the default installer).

### Which program needs the permission

macOS attributes the permission to the *responsible process*:

* Started by the LaunchAgent (the normal case): the `keyboard-chatter-filter` binary itself.
* Started from a terminal (`keyboard-chatter-filter run`): the terminal application (Terminal,
  iTerm2, VS Code…). The filter does not show the system prompt in that case, to avoid asking you to
  trust your terminal; it logs instructions instead.

### Updates and code signing

macOS remembers the permission for a specific code identity:

* **Developer ID-signed releases** keep the permission across updates (the identity is the signing
  team plus the identifier `keyboard-chatter-filter`).
* **Ad-hoc-signed releases** (the default when the release workflow has no Apple signing secrets)
  are identified by their exact code hash, so after every update you must switch the permission
  off and on again, or remove the old entry and re-add the binary.

## Gatekeeper and notarization

* The installer downloads with `curl`, which does not attach the quarantine attribute, so
  Gatekeeper does not block the binary. The download is verified against the release's SHA-256
  checksums (and optionally the GitHub build-provenance attestation).
* A binary downloaded with a web browser *is* quarantined. Unless the release is Developer ID-signed
  and notarized, macOS will refuse to run it ("cannot be verified"). Use the install script or build
  from source instead.
* The release workflow signs with a Developer ID and notarizes automatically when the
  `MACOS_*` repository secrets are configured (see [releasing.md](releasing.md)). Bare executables
  cannot be stapled, so Gatekeeper fetches the notarization ticket online the first time.
* On Apple silicon every binary must carry at least an ad-hoc signature; release binaries always
  do.

## Event handling details

* The tap sits at the HID level (`kCGHIDEventTap`, head of the chain), before session-level tools and
  every application, and listens to `keyDown`, `keyUp` and `flagsChanged` only. Mouse, media keys
  (system-defined events) and everything else are never touched.
* Modifier keys arrive as `flagsChanged`; whether the key went down or up is read from its
  left/right-specific flag bit, so the two Shift (Command, Option, Control) keys are distinguished.
* **Caps Lock is not filtered**: macOS toggles the lock state inside the HID system before any event
  tap sees the event, so dropping it would only desynchronise the state.
* Key repeat is recognised from `kCGKeyboardEventAutorepeat`.
* Only events from the HID system are filtered. Software-generated events (password managers'
  auto-type, remote desktop, automation tools) pass untouched.
* Event timestamps are converted to nanoseconds of `mach_absolute_time` whether the system reports
  them as nanoseconds or as mach ticks.
* If macOS disables the tap (callback too slow, or *Secure Event Input* is active), the filter
  re-enables it and resets its per-key state.

## Limitations

* **Secure Event Input.** While a password field (or Terminal's *Secure Keyboard Entry*) is focused,
  macOS withholds keyboard events from all event taps. Chatter is not filtered there.
* **Fast user switching**: each logged-in user runs their own instance through their own
  LaunchAgent.
* If the filter process is killed while a key-up is being held back (≤ 30 ms window), that one
  key-up is lost; applications that track key state may consider the key held until it is pressed
  again. The keyboard itself keeps working: macOS removes a dead process's event tap automatically.

## Manual verification checklist

Hosted CI cannot grant Accessibility access, so the end-to-end path is verified by hand:

1. `bash install.sh` (or the curl one-liner); grant Accessibility when prompted.
2. `keyboard-chatter-filter status` → `State: active`.
3. Type normally in TextEdit, including fast double letters ("bookkeeper") and Shift-capitalised
   words; hold a letter to auto-repeat; hold an arrow key.
4. With a chattering keyboard (or a test keyboard), confirm doubled letters disappear and
   `status` shows a growing *Suppressed* count.
5. Log out and in, or reboot: `status` → active again.
6. `keyboard-chatter-filter uninstall`; `launchctl print gui/$(id -u)/keyboard-chatter-filter`
   reports the service is not found.
