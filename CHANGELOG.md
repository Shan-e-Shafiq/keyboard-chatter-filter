# Changelog

All notable changes are documented here. The project follows [Semantic Versioning](https://semver.org).

## [0.1.0] - unreleased

First release.

* Per-key chatter filter: immediate key-down; on Linux held-back key-ups with chatter cancellation
  and order-preserving delivery; on macOS/Windows immediate key-ups with no event ever created;
  duplicate-transition and orphaned-repeat removal.
* Safety circuit breaker: filtering switches itself off if it keeps dropping a key's deliberate
  presses. Event timestamps are only trusted when plausible. `run --dry-run` observes without
  dropping anything. (A pre-release macOS build that re-posted held-back key-ups made keys unusable
  on a real machine; these changes make that class of failure impossible or self-correcting.)
* macOS: HID-level Quartz event tap, LaunchAgent with automatic restart, Accessibility permission
  handling.
* Windows: low-level keyboard hook in a per-session agent supervised by an automatic-start Windows
  service.
* Linux: evdev grab + uinput virtual keyboard per physical keyboard, hot-plug, hardened systemd
  service with watchdog.
* Configuration file with validation and safe fallbacks; CLI for status/start/stop/restart/reload/
  config/install/uninstall.
* Installers with SHA-256 verification; release workflow with provenance attestations and optional
  code signing.
