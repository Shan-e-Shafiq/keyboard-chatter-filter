# keyboard-chatter-filter

A small background utility for macOS, Windows and Linux that removes the duplicate keystrokes a
worn or faulty keyboard switch produces ("chatter"). You install it once from the command line, and
it starts automatically every time you log in or boot.

> **Privacy.** This program intercepts keyboard events solely to remove switch chatter. Every
> keystroke is examined in memory for a few microseconds and then passed on or dropped as a
> duplicate (on Linux a key-up may be held back for up to 30 ms). **Nothing you type is ever recorded, logged, stored or
> transmitted.** The program contains no networking code, telemetry or analytics; on Linux the
> service runs with networking disabled by systemd. Its logs contain only start/stop messages,
> errors and counts. See [Security and privacy](#security-and-privacy).

---

- [What is keyboard chatter?](#what-is-keyboard-chatter)
- [What this does (and does not do)](#what-this-does-and-does-not-do)
- [Supported platforms](#supported-platforms)
- [Installation](#installation)
- [Automatic startup](#automatic-startup)
- [Required permissions](#required-permissions)
- [Configuration](#configuration)
- [Commands](#commands)
- [How the filter works](#how-the-filter-works)
- [Troubleshooting](#troubleshooting)
- [Uninstalling](#uninstalling)
- [Security and privacy](#security-and-privacy)
- [Limitations](#limitations)
- [Building from source and development](#building-from-source-and-development)

## What is keyboard chatter?

Every key on a keyboard is a small mechanical switch. When a switch is worn, dirty, or simply
defective, its contacts can bounce or briefly lose contact, so one press is reported to the
computer as two or more presses. The result is doubled letters ("thhe", "helllo"), extra spaces or
backspaces, or a held key that seems to let go for a moment. This is called **key chatter**. It is
common on aging mechanical keyboards and some low-travel laptop keyboards, and it can affect a
single key or several.

## What this does (and does not do)

The filter sits between your keyboard and your applications and removes the *extra* key events
that chatter produces. A key that is released and pressed again within a few milliseconds is
treated as one keystroke. A human cannot physically lift a finger and press the same key again that
quickly, while a bouncing switch does exactly that.

It does **not** repair a damaged switch. It filters duplicate input events, which may reduce or
eliminate the *visible effects* of chatter. If a switch is so worn that a press is sometimes not
registered at all, or the duplicates arrive more slowly than the configured threshold, filtering
cannot help, and cleaning or replacing the switch is the real fix.

Normal typing is not affected: fast double letters, key repeat when holding a key, shortcuts and
chords, and modifier keys all keep working.

## Supported platforms

| OS | Architectures | Interception | Starts automatically via |
|---|---|---|---|
| macOS 11+ | Apple silicon, Intel | Quartz event tap | per-user LaunchAgent |
| Windows 10/11 | x64, ARM64 | low-level keyboard hook | Windows service + per-session agent |
| Linux (kernel 4.5+, systemd) | x86_64, arm64 | evdev grab + uinput virtual keyboard | systemd service |

Built-in, USB and Bluetooth keyboards are all handled the same way.

## Installation

Prebuilt binaries are published on the GitHub Releases page; installation does not compile
anything. The installers download the release for your OS and CPU, **verify its SHA-256
checksum**, install it, register the background service, start it, and check that it is running.

### macOS and Linux

```sh
curl -fsSL https://raw.githubusercontent.com/Shan-e-Shafiq/keyboard-chatter-filter/main/install.sh | bash
```

* **macOS** installs to `~/.local/bin` and needs no administrator password. Afterwards, allow
  **Accessibility** access when macOS asks (see [permissions](#required-permissions)).
* **Linux** installs to `/usr/local/bin` and asks for your password through `sudo`, because the
  system service must run as root (in a tightly restricted sandbox).

Prefer to read the script first? Download it, review it, then run it:

```sh
curl -fsSLO https://raw.githubusercontent.com/Shan-e-Shafiq/keyboard-chatter-filter/main/install.sh
less install.sh
bash install.sh
```

### Windows

In **PowerShell opened with "Run as administrator"**:

```powershell
irm https://raw.githubusercontent.com/Shan-e-Shafiq/keyboard-chatter-filter/main/install.ps1 | iex
```

This installs to `C:\Program Files\keyboard-chatter-filter`, adds it to the system `PATH`, and
registers and starts the service. To review the script first, run
`irm <url> -OutFile install.ps1`, read it, then run `.\install.ps1`.

### Installer options

| Variable | Meaning |
|---|---|
| `KCF_REPO=owner/repo` | install from a fork (default `Shan-e-Shafiq/keyboard-chatter-filter`) |
| `KCF_VERSION=v1.0.0` | a specific release instead of the latest |
| `KCF_INSTALL_DIR=/path` | binary location (macOS/Linux) |
| `KCF_VERIFY_ATTESTATION=1` | also verify the GitHub build-provenance attestation (needs the `gh` CLI, logged in with `gh auth login`) |
| `KCF_ARCHIVE_DIR=/path` | offline install from a directory containing the release archive and `SHA256SUMS` |
| `KCF_REQUIRE_SIGNATURE=1` | Windows: refuse binaries without a valid Authenticode signature |

Example: `curl -fsSL …/install.sh | KCF_VERSION=v1.0.0 bash`.

The URL is the repository's raw `main` branch, so a fork only has to change the repository in that
URL (and `KCF_REPO`, or the default in the scripts). Every release also attaches copies of
`install.sh` and `install.ps1` that are pinned to the repository they were built from.

### Verifying a release manually

Each release contains `SHA256SUMS` and a [build provenance
attestation](https://docs.github.com/actions/security-for-github-actions/using-artifact-attestations)
proving that the files were built by this repository's release workflow:

```sh
sha256sum --check --ignore-missing SHA256SUMS        # macOS: shasum -a 256 --check --ignore-missing SHA256SUMS
gh attestation verify keyboard-chatter-filter-linux-x86_64.tar.gz --repo Shan-e-Shafiq/keyboard-chatter-filter
```

(`gh attestation verify` needs a logged-in `gh`, even for public repositories.)

## Automatic startup

After installation you do not need to start anything again:

* **macOS**: a LaunchAgent (`~/Library/LaunchAgents/keyboard-chatter-filter.plist`) starts the
  filter when you log in, and launchd restarts it if it ever crashes.
* **Windows**: the `keyboard-chatter-filter` service starts at boot, launches a filter agent in
  your session when you log on, and is restarted by Windows if it fails.
* **Linux**: the `keyboard-chatter-filter` systemd service starts at boot and is restarted if it
  fails; a watchdog restarts it if it ever hangs.

`keyboard-chatter-filter stop` stops filtering until the next login/boot or `start`; to stop it
permanently, uninstall it or set `enabled = false` in the configuration.

## Required permissions

| OS | What is needed | Why |
|---|---|---|
| macOS | **Accessibility** permission for `keyboard-chatter-filter` (System Settings → Privacy & Security → Accessibility). No administrator rights. | macOS only lets trusted processes drop or re-post keyboard events system-wide. Apps cannot grant this to themselves; you switch it on once. |
| Windows | Administrator rights **to install** the service. | The filter agent itself runs with your normal user rights. |
| Linux | Root **to install**; the service runs as root without capabilities, restricted to input devices. | Only root can read keyboards (`/dev/input/event*`) and create the filtered virtual keyboard (`/dev/uinput`). |

Until the macOS permission is granted, your keyboard keeps working normally, just unfiltered. The
filter starts by itself within seconds of the permission being granted. Details:
[macOS](docs/platform-macos.md#permissions), [Windows](docs/platform-windows.md#permissions),
[Linux](docs/platform-linux.md#permissions).

## Configuration

The configuration file is created on installation:

| OS | Path |
|---|---|
| macOS | `~/.config/keyboard-chatter-filter/config.toml` |
| Windows | `C:\ProgramData\keyboard-chatter-filter\config.toml` (edit as administrator) |
| Linux | `/etc/keyboard-chatter-filter/config.toml` (edit as root) |

```toml
# A key released and pressed again within this many milliseconds is chatter. Range 1-200.
chatter_threshold_ms = 30

# false = keep the service running without filtering anything.
enabled = true

# "error", "warning", "info" or "debug". Keystrokes are never logged at any level.
log_level = "info"

# Linux only: never intercept devices whose name contains one of these strings.
# ignored_devices = ["YubiKey", "Barcode"]
```

Apply changes with `keyboard-chatter-filter reload` (Windows: from an elevated terminal), or run
`keyboard-chatter-filter config` to see the file in use, the effective values and any problems.

Invalid settings never stop the filter: each problem is reported with its line number in
`config` and in the log, and that setting falls back to its default. A missing file means
defaults.

**Choosing a threshold.** 30 ms removes typical chatter with a wide margin below the fastest
intentional same-key double tap. If doubled letters still appear, raise it in steps of 10 ms
(above 80 ms you may start to merge intentional fast double letters). If you lose intentional
double taps, lower it.

## Commands

```
keyboard-chatter-filter status              show whether the filter runs and what it is doing
keyboard-chatter-filter start               start the background service
keyboard-chatter-filter stop                stop it (until the next login/boot or `start`)
keyboard-chatter-filter restart             restart it
keyboard-chatter-filter reload              re-read the configuration file
keyboard-chatter-filter config              show configuration path, effective settings and problems
keyboard-chatter-filter install             register the service for this binary (done by the installer)
keyboard-chatter-filter uninstall [--purge] remove the service and binary (--purge: also config and logs)
keyboard-chatter-filter run [--log-level debug]
                                            run in the foreground (for debugging; the service runs this)
keyboard-chatter-filter version
```

The commands talk to the running service; they never start a second filter. Only one filter
instance can run at a time (per user on macOS, per session on Windows, per system on Linux).

On Linux, `start`/`stop`/`restart`/`reload`/`install`/`uninstall` need `sudo`. On Windows,
`install`/`uninstall`/`start`/`stop`/`reload` need an elevated terminal.

## How the filter works

The filter tracks every key separately and looks at the **gap between a key's release and its next
press**. A re-press of the same key within the threshold (30 ms) is a bounce, and the duplicate
keystroke is removed.

* **Key-downs are delivered immediately**, so no delay is added when you type.
* **macOS and Windows**: key-ups also pass immediately, and a bounce is removed together with its
  key-up. The filter never creates or re-posts events, so it can only remove complete
  press/release pairs. Modifier keys are left untouched.
* **Linux**: key-ups are held back for the threshold. If the key bounces back down, both events are
  dropped and the key simply stays held, so long presses and key repeat survive a bouncing switch.
  Held-back releases are delivered before any other key's event, so order is preserved.
* Auto-repeat of held keys, intentional fast double letters (> 30 ms apart), chords and other keys
  are never affected.
* Input generated by software (password managers, remote desktop, automation tools) is never
  filtered.
* **Safety circuit breaker**: real chatter only removes bounces milliseconds apart; the next
  deliberate press always gets through. If the filter ever keeps dropping a key's deliberate
  presses, it switches filtering off by itself and lets every key through until restarted
  (`status` shows *SAFETY STOP*).
* `keyboard-chatter-filter run --dry-run` shows what the filter *would* remove without removing
  anything.

The full state machine, the reasoning behind it and the test strategy are in
[docs/filter-algorithm.md](docs/filter-algorithm.md).

## Troubleshooting

Start with `keyboard-chatter-filter status`. It shows the state, the threshold, how many chatter
keystrokes were removed, and where the log is.

| Symptom | What to do |
|---|---|
| macOS: `waiting-for-permission` | Allow Accessibility for `keyboard-chatter-filter` (System Settings → Privacy & Security → Accessibility). If it is listed and on but still waiting, switch it off and on again (needed after each update of an ad-hoc-signed release), or remove it with **−** and run `keyboard-chatter-filter restart`. |
| macOS: "cannot be opened because the developer cannot be verified" | You downloaded the binary with a browser. Use the install script, or build from source. See [Gatekeeper](docs/platform-macos.md#gatekeeper-and-notarization). |
| Doubled letters still appear | Raise `chatter_threshold_ms` (e.g. 40, 50) and `reload`. If *Suppressed* in `status` stays at 0, check that the filter is `active`. |
| An intentional fast double tap is lost | Lower `chatter_threshold_ms` (e.g. 20). |
| A YubiKey / barcode scanner drops characters | Linux: add its name to `ignored_devices`. macOS/Windows: give the device an inter-key delay (e.g. `ykman otp settings --pacing …`), or `stop` the filter while using it. |
| Linux: `active - no keyboard found yet` | Check `journalctl -u keyboard-chatter-filter` for skipped devices and the reason. Another program (keyd, kmonad…) may already grab the keyboard. |
| Linux: `cannot create the virtual keyboard` | `sudo modprobe uinput`, then `sudo keyboard-chatter-filter restart`. |
| Windows: *Filtering this session: no* | `keyboard-chatter-filter restart` from an elevated terminal; check `%LOCALAPPDATA%\keyboard-chatter-filter\agent.log`. |
| `status` shows *SAFETY STOP* | The filter detected that it was dropping deliberate key presses and switched itself off; the keyboard is unfiltered. Please open an issue with the log. `restart` re-arms it. |
| Unsure whether the filter suits your keyboard | Run a dry run (`run --dry-run`, see the platform docs). It shows what would be removed, and the *gaps* histogram in `status` shows how far apart the removed presses were (real chatter is mostly under 10 ms). |
| Something is wrong and you need the keyboard back right now | `keyboard-chatter-filter stop`. On every platform, a stopped or crashed filter leaves the keyboard working normally. |

Logs: macOS `~/Library/Logs/keyboard-chatter-filter/`, Windows
`C:\ProgramData\keyboard-chatter-filter\logs\` and `%LOCALAPPDATA%\keyboard-chatter-filter\`,
Linux `journalctl -u keyboard-chatter-filter`. For more detail, set `log_level = "debug"` and
`reload`. Even debug logs contain no keystrokes.

## Uninstalling

```sh
keyboard-chatter-filter uninstall            # macOS (no sudo needed)
sudo keyboard-chatter-filter uninstall       # Linux
keyboard-chatter-filter uninstall            # Windows, from an elevated terminal
```

This stops the service, removes the startup registration and the installed binary, and leaves your
configuration file and logs in place; add `--purge` to remove those too. It only deletes files this
project created, and running it again is harmless.

If the binary is already gone, use the standalone scripts (also attached to every release):
`bash scripts/uninstall.sh [--purge]` on macOS/Linux, `.\scripts\uninstall.ps1 [-Purge]` on Windows
(elevated). On macOS you can also remove the entry from the Accessibility list afterwards.

## Security and privacy

A program that sees every keystroke has to be trustworthy by construction. These are the design
rules this project follows, and how they are enforced:

* **No recording.** Keystrokes exist only as transient in-memory events while they are being
  decided. There is no key log, no history buffer, and no file or socket that receives key data.
  The only per-key memory is timing state (last press/release time) needed to detect chatter.
  Logs and `status` report counts and settings only, never which keys were pressed.
* **No network.** The program contains no networking code at all, and there is no telemetry,
  update check or analytics. On Linux, systemd additionally runs it with `PrivateNetwork=yes` and
  `IPAddressDeny=any`. The only local IPC is a status socket that sends a snapshot of counters and
  never reads client input.
* **Least privilege.** macOS: runs as you, with only the Accessibility permission. Windows: the
  filter runs as you, and only the small supervisor service runs as LocalSystem. Linux: root with
  an empty capability set, access to input devices only, a read-only filesystem and a syscall filter
  (`systemd-analyze security` exposure **1.1/10**).
* **Software input is never touched**, so password managers and accessibility tools behave as
  before.
* **Fail open.** The filter never creates keyboard events on macOS and Windows, so it cannot leave a
  key stuck, and a safety circuit breaker switches filtering off if it ever starts dropping
  deliberate presses. If the filter crashes, hangs or is stopped, the OS removes its interception
  and the keyboard keeps working unfiltered: macOS removes a dead process's event tap; Windows removes a
  dead process's hook (and skips a hung one); Linux releases the grab and the systemd watchdog
  kills a hung filter. This is covered by automated tests on Linux.
* **Safe files and commands.** Configuration and logs are created with owner-only permissions
  (macOS), a protected ACL (Windows) or root ownership (Linux). The configuration parser validates
  every value and rejects oversized files. External tools (`launchctl`, `systemctl`) are run
  directly with argument lists and absolute paths, never through a shell. The Linux and Windows
  installers refuse to register a root/SYSTEM service for a binary that unprivileged users could
  replace.
* **Verified downloads.** Installers use HTTPS only and verify SHA-256 checksums; releases carry
  GitHub build-provenance attestations, and are code-signed when signing keys are configured.

Please report vulnerabilities privately, as described in [SECURITY.md](SECURITY.md).

## Limitations

* **Threshold-based.** Bounces slower than the threshold are not caught, and intentional same-key
  re-presses faster than the threshold are merged. The default suits typical chatter; adjust it if
  needed.
* **Key-up latency (Linux).** Releases reach applications up to the threshold (30 ms) late, unless
  another key is pressed first. Typing is unaffected; games that act on key release see this delay.
  macOS and Windows add no latency.
* **Held keys (macOS, Windows).** If a held key's switch bounces, applications see it released
  early and its auto-repeat stops for that press. Chatter on modifier keys is not filtered there.
* **Machine-typed input**, such as YubiKey one-time passwords and barcode scanners, can repeat a
  key within milliseconds, which looks exactly like chatter. Linux skips YubiKeys automatically and
  supports `ignored_devices`. On macOS and Windows the OS does not tell the filter which device a
  key came from, so give such devices an inter-character delay or stop the filter while using them.
* **macOS**: not filtered while *Secure Event Input* is active (password fields, Terminal's *Secure
  Keyboard Entry*), because macOS hides keystrokes from all event taps then. Ad-hoc-signed releases
  need the Accessibility permission re-enabled after each update.
* **Windows**: input to elevated (administrator) windows and the secure desktop is not filtered. A
  kernel driver would lift this limit; the architecture allows one, but none is shipped.
* **Linux**: tools that also grab keyboards (keyd, kmonad, interception-tools) conflict; the first
  to grab a keyboard owns it. Per-device desktop settings must name the virtual keyboard. Requires
  systemd for automatic start (other init systems can run `keyboard-chatter-filter run`).
* Verified automatically: the filtering core on every platform; the Linux adapter against the real
  kernel input stack. The macOS and Windows adapters compile and are unit-tested in CI, but their
  end-to-end behaviour needs a logged-in desktop (and, on macOS, a permission grant), so it is
  verified manually. See the checklists in [docs/](docs/).

## Building from source and development

Requirements: CMake 3.21+, Ninja (or Visual Studio 2022 on Windows), and a C++20 compiler: Apple
Clang 14+ (Xcode command line tools), GCC 12+, Clang 15+ or MSVC 19.3x. No third-party libraries
are needed; Linux needs the kernel headers (`linux-headers`/`linux-libc-dev`).

```sh
cmake --preset release          # or: debug, ci (warnings as errors), asan
cmake --build --preset release
ctest --preset release
./build/release/keyboard-chatter-filter run --log-level debug   # foreground, Ctrl+C to stop
```

Windows (Developer PowerShell): `cmake --preset windows-release`, `cmake --build --preset
windows-release`, `ctest --preset windows-release`.

To install a locally built binary as a service, copy it to the standard location and run its
`install` command (macOS: anywhere you like; Linux: a root-owned directory such as
`/usr/local/bin`; Windows: `C:\Program Files\keyboard-chatter-filter\`).

Linux kernel integration tests (need root and the `uinput` module; they only ever touch a synthetic
test keyboard):

```sh
sudo modprobe uinput
sudo ./build/release/tests/linux_uinput_integration_tests
```

Project layout:

```
include/keyboard_filter/   public core API: KeyEvent, ChatterFilter, FilterEngine, Configuration, interfaces
src/core/                  platform-independent filtering and configuration (no OS headers)
src/common/                logging, CLI parsing, status protocol, POSIX helpers
src/macos/                 event tap, output, LaunchAgent management, daemon
src/windows/               low-level hook, SendInput output, service + per-session agent
src/linux/                 evdev/uinput adapter, device classification, systemd management, daemon
src/app/main.cpp           command-line entry point
tests/                     unit, property and Linux kernel integration tests
packaging/                 LaunchAgent and systemd templates (embedded into the binary at build time)
scripts/                   uninstall scripts, CI build/sign helpers
docs/                      architecture, algorithm, per-platform and release documentation
```

Further reading: [architecture](docs/architecture.md) ·
[filter algorithm](docs/filter-algorithm.md) · [macOS](docs/platform-macos.md) ·
[Windows](docs/platform-windows.md) · [Linux](docs/platform-linux.md) ·
[releasing](docs/releasing.md) · [contributing](CONTRIBUTING.md).

## License

[MIT](LICENSE)
