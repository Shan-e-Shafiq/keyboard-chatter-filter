# Linux

Supported: x86_64 and arm64, kernel 4.5 or newer, with systemd for automatic startup. The release
binary is fully static (musl) and runs on any distribution. It works below X11, Wayland and the text
console alike, because it filters at the kernel input layer.

## How it works

```
physical keyboard ─▶ /dev/input/eventN ──(EVIOCGRAB: exclusive)──▶ keyboard-chatter-filter
                                                                        │ filtered events
                                                                        ▼
                   applications ◀── libinput / X / console ◀── /dev/uinput virtual keyboard
```

* Every physical keyboard is **grabbed** with `EVIOCGRAB`, so the desktop no longer receives its
  events directly, and mirrored by a **uinput** virtual keyboard named
  `keyboard-chatter-filter: <original name>` with the same keys, LEDs and vendor/product IDs.
  Applications only ever see the filtered stream, never both.
* The grab is taken only once no key on the device is held (otherwise the desktop would see a press
  without its release).
* If the filter exits or crashes for any reason, the kernel releases the grab and destroys the
  virtual keyboard at once, and typing continues on the unfiltered keyboard. If the event loop ever
  hangs, the systemd watchdog (20 s) kills it, with the same result. This is tested in CI
  (`linux_uinput_integration_tests`).
* Keyboard LEDs (Caps Lock…) set on the virtual keyboard are mirrored to the physical one.
* Kernel auto-repeat of the physical keyboard is forwarded; the virtual keyboard does not enable its
  own repeat, so the console does not get doubled repeats. Desktop environments generate repeat
  themselves, as usual.

### Device discovery

Devices are never assumed by number (`/dev/input/event0` is often the power button). Every
`/dev/input/event*` node is inspected and filtered only if:

* it reports key events, including at least 20 letter keys plus Space and Enter, or a complete
  numeric keypad; and
* it has no pointer capabilities (`BTN_LEFT`, `BTN_TOUCH`, pen/finger tools, `REL_X/Y`,
  `ABS_X/Y`) and no joystick/gamepad buttons.

Never filtered: mice, touchpads, tablets, touchscreens, game controllers, power/sleep buttons,
media-key-only interfaces, **virtual devices** (`BUS_VIRTUAL`: ydotool, remote desktop, our own
virtual keyboards), **YubiKeys** (vendor 0x1050; their one-time passwords repeat characters within
milliseconds), and any device whose name contains an entry of `ignored_devices` in the
configuration.

New keyboards (USB plug-in, Bluetooth reconnect) are picked up through inotify on `/dev/input`;
unplugged ones are released.

## Installation layout

| File | Purpose |
|---|---|
| `/usr/local/bin/keyboard-chatter-filter` | binary (root-owned; `install` refuses binaries in user-writable locations) |
| `/etc/systemd/system/keyboard-chatter-filter.service` | systemd unit (from `packaging/linux/keyboard-chatter-filter.service.in`) |
| `/etc/keyboard-chatter-filter/config.toml` | configuration |
| `/etc/modules-load.d/keyboard-chatter-filter.conf` | loads `uinput` at boot |
| `/run/keyboard-chatter-filter/` | lock file and status socket (created by systemd) |
| journal | logs: `journalctl -u keyboard-chatter-filter` |

## Permissions

Reading `/dev/input/event*` and creating devices through `/dev/uinput` is restricted to root on most
distributions (`/dev/input/event*` is usually `root:input 0660`, `/dev/uinput` `root:root 0600`).
Anyone able to read these devices can log every keystroke, and anyone able to write uinput can type
as you, so this access must not be handed to ordinary users. The service therefore runs as root,
but confined by systemd so that root has no power beyond these devices:

* `CapabilityBoundingSet=` (empty): root without any capability. Access works only because root owns
  the device nodes.
* `DevicePolicy=closed`, `DeviceAllow=/dev/uinput rw`, `DeviceAllow=char-input rw`: no other device.
* `PrivateNetwork=yes`, `IPAddressDeny=any`, `RestrictAddressFamilies=AF_UNIX`: no network at all.
* `ProtectSystem=strict`, `ProtectHome=yes`, `PrivateTmp=yes`: read-only system, no home directories.
* `NoNewPrivileges`, `SystemCallFilter=@system-service`, `MemoryDenyWriteExecute`,
  `RestrictNamespaces`, kernel/cgroup/clock protections.

`systemd-analyze security keyboard-chatter-filter` rates the unit **1.1 (OK)**.

The installer needs root (via `sudo`) to install the binary and the unit. `status` works for every
user; `start`, `stop`, `restart` and `reload` go through `systemctl` and therefore need root or a
polkit authorisation.

### Running without systemd

Run `keyboard-chatter-filter run` as root from your init system (OpenRC, runit, s6), with
`SIGTERM` to stop and `SIGHUP` to reload. Keep the same restrictions where your init system can
express them.

## Limitations and interactions

* **Other grabbing tools** (keyd, kmonad, kanata, interception-tools, evsieve) also grab keyboards.
  Whichever grabs a device first owns it. To combine them, either filter first and let the remapper
  read our virtual keyboard, or list the device in `ignored_devices` and use the remapper's own
  debounce.
* **Per-device desktop settings** (e.g. sway `input "<vendor>:<product>:<name>"` blocks, or per
  keyboard layouts) must refer to the virtual keyboard's name, `keyboard-chatter-filter: <name>`.
* A keyboard whose single event node also contains a pointing device (some keyboards with built-in
  trackpads expose both on one interface) is left alone rather than risking the pointer.
* Multi-seat setups: virtual keyboards are created on the default seat.

## Verification

* CI runs the kernel integration tests (`sudo` + `modprobe uinput` on GitHub's Ubuntu runners):
  chatter removal, double taps, modifier ordering, auto-repeat, graceful stop, and keyboard
  hand-back after `SIGKILL`.
* The installer and unit have been exercised end to end in a systemd container: install from a
  release archive, hot-plugging a keyboard, filtering under the sandbox, reload with valid and
  invalid configuration, stop/start/restart, crash → automatic restart, idempotent uninstall.
