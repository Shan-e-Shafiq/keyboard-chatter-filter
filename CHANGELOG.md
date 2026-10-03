# Changelog

All notable changes are documented here. The project follows [Semantic Versioning](https://semver.org).

## [0.1.0] - unreleased

First release.

* Per-key chatter filter: immediate key-down, held-back key-up with chatter cancellation,
  order-preserving delivery, duplicate-transition and orphaned-repeat removal.
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
