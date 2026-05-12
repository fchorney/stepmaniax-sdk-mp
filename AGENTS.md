# AGENTS.md

## Project overview

This is a cross-platform SDK for StepManiaX dance pads, based on the [original StepManiaX SDK](https://github.com/steprevolution/stepmaniax-sdk). The original SDK is included as a git submodule at `original_sdk/` for reference.

The goal is to eventually implement all features from the original SDK, but with a fundamentally different architecture optimized for input latency. All features from the original SDK have been implemented. Input processing (Report 3 packets) is the highest priority — it must be as fast and low-latency as possible. Report 6 data (commands, config, device info) must still be processed correctly but is secondary to input responsiveness.

## Architecture

This SDK uses a two-thread design (vs the original's single I/O thread):

- **USB polling thread** — runs at ~1ms intervals, reads HID data from both pads. Report 3 (input state) is parsed inline and updates an atomic variable immediately. Report 6 packets are buffered for the main thread.
- **Main I/O thread** — runs at ~50ms intervals, handles device discovery, connection management, configuration, and command processing. Consumes Report 6 data from the USB polling thread.

Key design decisions:
- Uses **hidapi** behind an abstraction layer (`IHIDDevice`/`IHIDEnumerator` interfaces in `SMXHIDInterface.h`). Only `SMXHIDInterface.cpp` touches hidapi directly, making the HID backend swappable and testable.
- Core logic is consolidated into **SMX.cpp** (helpers, SMXDevice, SMXManager, public API) rather than spread across many files.
- Only `SMX_*` public API symbols are exported; all internal symbols are hidden.
- C++14 standard, no higher.

## Code style

- 4-space indentation, no tabs.
- Opening braces on the same line for control flow (`if`, `for`, `while`), next line for function/class definitions.
- Member variables use `m_` prefix with type-hinting: `m_b` (bool), `m_i` (int), `m_s` (string), `m_p` (pointer), `m_a` (array/vector).
- Local variables use camelCase with similar type prefixes (`sError`, `iCount`, `bActive`).
- Functions and methods use PascalCase (`GetInputState`, `SendCommand`).
- Constants and macros use UPPER_SNAKE_CASE.
- `using namespace std;` is used within implementation files.
- Doxygen-style `///` comments for public/important functions with `@param`, `@return` tags.
- Section separators use `// ---` comment blocks with descriptive headers.
- Prefer `const` parameters and `lock_guard` for mutex management.
- No trailing whitespace. Files end with a newline.

## Dependencies

- **CMake** 3.14+
- **C++14** compiler
- **hidapi** — the only external library dependency
- **gif_load** — single-header animated GIF decoder (vendored in `src/vendor/`, public domain)

New dependencies should not be added unless there is a compelling reason. The SDK should remain minimal and easy to build on all platforms. Prefer vendored single-header libraries over external dependencies when the functionality is small and self-contained.

## Project structure

```
├── include/SMX.h                    # Public API header (only exported interface)
├── src/
│   ├── SMX.cpp                      # Helpers, SMXDevice, SMXManager, API implementation
│   ├── SMXDeviceConnection.h        # HID I/O class (header)
│   ├── SMXDeviceConnection.cpp      # HID I/O class (implementation)
│   ├── SMXHIDInterface.h            # HID abstraction interfaces
│   ├── SMXHIDInterface.cpp          # Real hidapi-backed implementation
│   ├── SMXHIDRecorder.h             # HID traffic record/replay
│   ├── SMXHIDRecorder.cpp           # HID traffic record/replay implementation
│   ├── SMXConfigPacket.h            # Internal config struct
│   ├── SMXConfigPacket.cpp          # Old firmware config format conversion
│   ├── SMXPanelAnimation.cpp        # GIF animation loading and playback
│   ├── SMXVersion.h.in              # Version header template (configured by CMake)
│   └── vendor/
│       └── gif_load.h              # Single-header GIF decoder (public domain)
├── tests/
│   ├── test_main.cpp                # Version and log callback tests
│   ├── test_device_connection.cpp   # Device connection tests with fake HID
│   ├── test_smx_manager.cpp         # Manager discovery and ordering tests
│   ├── test_config_packet.cpp       # Config format conversion tests
│   ├── test_config_api.cpp          # Config get/set API tests
│   ├── test_helpers.cpp             # Utility function tests
│   ├── test_helpers_manager.h       # Shared test infrastructure for manager-level tests
│   ├── test_move_semantics.cpp      # Move semantics / pad swap regression tests
│   ├── test_lights.cpp              # Panel LED control tests
│   ├── test_panel_animation.cpp     # GIF animation loading tests
│   ├── test_replay.cpp              # HID traffic replay regression tests
│   └── test_integration.cpp         # Integration tests (real hardware)
├── sample/sample.cpp                # Sample application
├── scripts/
│   └── decode_smxhid.py             # Decode .smxhid capture files for debugging
├── capture/                         # Recorded HID traffic for replay-based tests
├── bump-version.sh                  # Version bump helper script
├── original_sdk/                    # Original SDK (git submodule, reference only)
└── CMakeLists.txt                   # Build configuration
```

## Git conventions

- Branch names should be prefixed with your initials (e.g., `fc/add-sensor-data`).
- Prefer rebases over merges to keep history linear.
- Do not push directly to main.
- Code in commits should follow the style conventions above.

## Testing

The project uses [doctest](https://github.com/doctest/doctest) for unit testing, fetched automatically via CMake's FetchContent.

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make
ctest
```

The HID abstraction layer (`IHIDDevice`/`IHIDEnumerator`) enables testing without physical hardware. Tests inject a `FakeHIDDevice` that queues pre-built packets and captures writes, allowing full testing of packet parsing, state management, and connection logic.

Shared test helpers (fake devices, packet builders, utility functions) that serve multiple test files should be factored into their own header file (e.g., `test_helpers_manager.h`) rather than duplicated across test files. Avoid code duplication in tests just as you would in production code.

### HID traffic record/replay

The `SMXHIDRecorder` module (`src/SMXHIDRecorder.h/.cpp`) provides:

- **RecordingHIDDevice / RecordingHIDEnumerator** — decorator wrappers that log all HID reads/writes with timestamps to binary `.smxhid` capture files. Used during integration tests to capture real device traffic.
- **ReplayHIDDevice** — loads a `.smxhid` capture file and replays reads in order, allowing regression tests to run against real recorded traffic without hardware.
- **Binary format:** `"SMXHID\x01"` magic header + records of `[type:1][timestamp_us:8][size:2][data:size]`.

### Integration tests (real hardware)

Integration tests require a physical SMX pad and are gated behind a separate CMake flag:

```bash
cmake .. -DBUILD_INTEGRATION_TESTS=ON
make smx-integration-tests
./smx-integration-tests
```

Tests skip gracefully if no hardware is detected. To record HID traffic during integration tests for later replay-based regression testing:

```bash
SMX_CAPTURE_DIR=/tmp/captures ./smx-integration-tests
```

## Building

```bash
mkdir build && cd build
cmake .. -DBUILD_SAMPLE=ON
make
```

See README.md for platform-specific instructions and all build options.

## Key considerations when contributing

1. **Input latency is paramount.** Any change to the USB polling thread or Report 3 handling path must not add latency. The atomic update of input state must remain lock-free.
2. **Thread safety.** The two-thread model requires careful attention to which thread owns which data. See the threading documentation in `SMXDeviceConnection.h` for the full breakdown.
3. **Keep the public API minimal.** Only `SMX_*` functions are exported. Internal classes and helpers stay in the `SMX` namespace or anonymous namespaces.
4. **Cross-platform.** All code must build and work on Linux, macOS (Intel + Apple Silicon), and Windows. Use standard C++14 and hidapi — no platform-specific code in the core logic.
5. **Reference the original SDK.** When implementing new features, consult `original_sdk/` for protocol details and expected behavior, but don't copy its architecture decisions blindly.
6. **Keep the README.md and AGENTS.md up to date.** When changing code, make sure to update the README.md and AGENTS.md if necessary.
7. **Always write tests.** Every bug fix, new feature, or code change must be accompanied by corresponding tests. When fixing a bug, add a test that reproduces the bug first. When adding or modifying functionality, add or update tests to cover the new behavior. Tests are not optional.
