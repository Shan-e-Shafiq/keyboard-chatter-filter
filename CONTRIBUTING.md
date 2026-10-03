# Contributing

Thanks for helping! This project intercepts keyboard input, so correctness, safety and privacy come
before features. Please read [docs/architecture.md](docs/architecture.md) and
[docs/filter-algorithm.md](docs/filter-algorithm.md) first.

## Ground rules

* **Never log, store or transmit key data.** No key codes, characters or per-key timings in logs,
  status output, crash messages or test artefacts written outside the test process. Counts are
  fine.
* **No network code** in the runtime, and no new runtime dependencies without discussion.
* **Fail open.** Any error path must leave the keyboard working (pass the event through, release
  grabs, remove hooks). Never block input because of our own failure.
* **Filtering logic lives only in `src/core/`.** Adapters translate events; they never decide.
  The core must not include OS headers.
* **No allocations or blocking calls on the per-event path** (event tap callback, hook procedure,
  evdev read loop).
* Keep it small. Prefer a clear 20-line function over a configurable framework.

## Development setup

See [Building from source](README.md#building-from-source-and-development). Useful presets:

| Preset | Purpose |
|---|---|
| `debug` | day-to-day development |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer (Clang/GCC) |
| `ci` | Release with warnings as errors, as CI builds it |
| `windows-debug` / `windows-release` | Visual Studio 2022 |

Cross-check the Windows code from macOS/Linux with MinGW:

```sh
cmake -S . -B build/mingw -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64-x86_64.cmake
cmake --build build/mingw
```

Build the static Linux release binary exactly like CI (any host with Docker):

```sh
docker run --rm -v "$PWD":/src -w /src alpine:3.20 sh scripts/ci/build-linux-static.sh x86_64
```

## Tests

* Every change to `src/core/` needs tests in `tests/chatter_filter_tests.cpp` or
  `tests/configuration_tests.cpp`. Drive the filter through `tests/fake_keyboard.h`, which shows
  exactly what applications would receive.
* Adapter translation rules (`event_conversion`, `key_mapping`, `device_classifier`) are pure and
  tested in `tests/adapter_logic_tests.cpp` on all platforms. Keep OS calls out of them.
* Linux adapter changes: run `sudo ./build/<preset>/tests/linux_uinput_integration_tests`.
* macOS/Windows adapter changes: run through the manual checklist in the platform document and
  describe what you verified in the pull request.

## Style

* C++20, `.clang-format` (Google-based, 4-space indent, 120 columns), `.editorconfig`.
* RAII for every OS resource; `std::unique_ptr` where ownership is needed; no raw `new`/`delete`.
* Comments explain *why*, not *what*.
* Shell scripts must pass `shellcheck`; PowerShell must parse on Windows PowerShell 5.1.

## Pull requests

* One topic per PR, with a description of the behaviour change and how you tested it.
* CI must be green on all platforms.
* Security-relevant changes (interception, privileges, installers, release pipeline) get an extra
  careful review; please call them out.
