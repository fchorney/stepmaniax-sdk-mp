# StepManiaX SDK Multi Platform

A minimal, cross-platform SDK for StepManiaX dance pads. This is a library only — it does not include a configuration tool or GUI. Applications and tools can be built on top of it.

## Features

- Auto-discovers up to 2 connected SMX pads via USB HID
- Handles connect, disconnect, and reconnect
- Identifies P1 vs P2 and auto-corrects pad ordering
- Reads panel press/release state as a low-latency bitmask
- Full device configuration (thresholds, lighting, calibration)
- Sensor diagnostics (raw/calibrated values, noise, tare)
- Platform LED strip control
- Cross-platform: Linux, macOS (Intel & Apple Silicon), Windows

## Feature status

Comparison of features between this SDK and the original StepManiaX SDK.

### Implemented

| Feature | API | Notes |
|---------|-----|-------|
| Device discovery & connection | `SMX_Start`, `SMX_Stop` | Auto-discovers up to 2 pads |
| Log callback | `SMX_SetLogCallback` | |
| Device info | `SMX_GetInfo` | Connection status, serial, firmware version, player ID |
| Input state | `SMX_GetInputState` | Panel press/release bitmask |
| Serial number assignment | `SMX_SetSerialNumbers` | Writes random serial to devices without one |
| SDK version | `SMX_Version` | |
| Polling rate configuration | `SMX_SetPollingRate` | *New* — not in original SDK |
| Input state callback mode | `SMX_SetInputStateMode` | *New* — fire on every packet or only on change |
| Monotonic time | `SMX_GetMonotonicTime` | *New* — high-resolution elapsed time |
| Factory reset | `SMX_FactoryReset` | Reset pad to default configuration |
| Force recalibration | `SMX_ForceRecalibration` | Trigger immediate sensor recalibration |
| Re-enable auto lights | `SMX_ReenableAutoLights` | Return panels to automatic step lighting |
| Panel test mode | `SMX_SetPanelTestMode` | Panel-side diagnostic lighting (pressure test) |
| Get/set configuration | `SMX_GetConfig`, `SMX_SetConfig` | Read/write pad thresholds, lighting config, sensor settings |
| Platform LED strip | `SMX_SetPlatformLights` | Control the platform edge LED strip (firmware v4+) |
| Panel LED control | `SMX_SetLights2` | Set RGB colors for all panel LEDs (up to 30 FPS) |
| GIF animation playback | `SMX_LightsAnimation_Load`, `SMX_LightsAnimation_SetAuto` | Load and auto-play GIF animations on panels |
| Animation upload | `SMX_LightsUpload_PrepareUpload`, `SMX_LightsUpload_BeginUpload` | Upload animations to firmware EEPROM for offline playback |
| Sensor test mode | `SMX_SetTestMode`, `SMX_GetTestData` | Read raw/calibrated sensor values for diagnostics |

### Not yet implemented

All features from the original SDK have been implemented.

## Threading architecture

This SDK uses a two-thread design that differs from the original StepManiaX SDK. The original SDK uses a single I/O thread for all USB communication. This version separates input state polling into its own dedicated thread to minimize input latency.

- **USB polling thread** — continuously reads HID data from both pads. Input state packets (Report 3) are parsed inline and update an atomic variable immediately, ensuring the lowest possible latency for panel press/release detection. Non-input packets (Report 6) are buffered for the main thread.
- **Main I/O thread** — handles device discovery, connection management, configuration, and command processing. Wakes on Report 6 data from the USB polling thread or on a configurable timeout.

Both thread sleep intervals are configurable via `SMX_SetPollingRate(int mainThreadMs, int usbPollingUs)`. The USB polling thread defaults to 1000µs between cycles; the main thread defaults to 50ms.

### Device report rate

The USB polling thread reads as fast as the device sends data, but the actual input report rate is determined by the pad's firmware — not the SDK's polling interval. Based on observation (the pad firmware is not open source):

- **Idle:** ~10 Report 3 packets/sec (likely a periodic heartbeat)
- **Active input:** ~50 packets/sec during panel presses (one report per state transition)

This suggests the pad only sends input reports when the panel state changes or on a low-frequency heartbeat, rather than continuously streaming at the USB endpoint's maximum rate. The SDK's polling interval only needs to be fast enough to not miss packets between reads — it does not control how often the device sends them.

## Dependencies

- **CMake** 3.14+
- **C++14** compiler
- **[hidapi](https://github.com/libusb/hidapi)** — lightweight HID library
- **[gif_load](https://github.com/hidefromkgb/gif_load)** — single-header animated GIF decoder (vendored, public domain)

### Vendored libraries

The `src/vendor/` directory contains single-header libraries that are included directly in the source tree rather than fetched externally:

| Library | File | License | Purpose |
|---------|------|---------|---------|
| gif_load | `src/vendor/gif_load.h` | Public domain (Unlicense) | Animated GIF decoding for panel animations |

We chose `gif_load` over rolling our own GIF decoder (as the original SDK does) because:
- It's ~200 lines of battle-tested C code vs ~400 lines of custom LZW implementation
- It handles edge cases the original SDK doesn't (interlacing, partial GIFs)
- GIF is a frozen spec (unchanged since 1989) so maintenance isn't a concern
- Public domain means zero license friction

## Original SDK source

The original StepManiaX SDK is included as a git submodule at `original_sdk/` for reference and comparison during development. To initialize it:

```bash
git submodule update --init
```

This is optional and not required to build the project.

## Building

### Linux (Debian/Ubuntu)

```bash
sudo apt update
sudo apt install build-essential cmake libhidapi-dev
```

```bash
cd stepmaniax-sdk-mp
mkdir build && cd build
cmake ..
make
```

#### USB permissions

By default, HID devices require root access. To allow your user to access SMX pads without `sudo`, create a udev rule:

```bash
sudo tee /etc/udev/rules.d/99-stepmaniax.rules <<EOF
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="2341", ATTRS{idProduct}=="8037", MODE="0666"
EOF
sudo udevadm control --reload-rules
sudo udevadm trigger
```

Unplug and replug the pad after applying the rule.

### Linux (Fedora/RHEL)

```bash
sudo dnf install gcc-c++ cmake hidapi-devel
```

Then follow the same build and udev steps as above.

### Linux (Arch)

```bash
sudo pacman -S base-devel cmake hidapi
```

Then follow the same build and udev steps as above.

### macOS (Intel & Apple Silicon)

Install dependencies via [Homebrew](https://brew.sh):

```bash
brew install cmake hidapi
```

```bash
cd stepmaniax-sdk-mp
mkdir build && cd build
cmake ..
make
```

This works on both Intel and Apple Silicon Macs. Homebrew installs native arm64 libraries on Apple Silicon and x86_64 on Intel. No special flags are needed — CMake will detect the correct architecture automatically.

**Note:** macOS may prompt you to allow the application to access USB devices. No additional permissions setup is required beyond granting that access.

### Windows

#### Option A: vcpkg

Install [vcpkg](https://github.com/microsoft/vcpkg) and [Visual Studio](https://visualstudio.microsoft.com/) (or the Build Tools with C++ workload).

```powershell
vcpkg install hidapi
```

```powershell
cd stepmaniax-sdk-mp
mkdir build && cd build
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

#### Option B: MSYS2/MinGW

Install [MSYS2](https://www.msys2.org/), then from the MSYS2 MinGW64 shell:

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-hidapi
```

```bash
cd stepmaniax-sdk-mp
mkdir build && cd build
cmake .. -G "MinGW Makefiles"
mingw32-make
```

**Note:** On Windows, no special driver or permissions setup is needed. The SMX pads use standard USB HID and work with the built-in HID driver.

### Build Options

By default, the build produces a **shared library** (`libsmx-mp.so` / `libsmx-mp.dylib` / `smx-mp.dll`). The sample application is **not** built by default.

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_SHARED_LIBS` | `ON` | Build shared library (set to `OFF` for static) |
| `BUILD_SAMPLE` | `OFF` | Build the sample application |
| `BUILD_TESTS` | `OFF` | Build unit tests |
| `BUILD_INTEGRATION_TESTS` | `OFF` | Build integration tests (require real hardware) |

```bash
cmake ..                                            # shared lib only (default)
cmake .. -DBUILD_SAMPLE=ON                          # shared lib + sample
cmake .. -DBUILD_SHARED_LIBS=OFF                    # static lib only
cmake .. -DBUILD_SHARED_LIBS=OFF -DBUILD_SAMPLE=ON  # static lib + sample
```

#### macOS architecture options

```bash
cmake .. -DCMAKE_OSX_ARCHITECTURES=arm64   # Apple Silicon only
cmake .. -DCMAKE_OSX_ARCHITECTURES=x86_64  # Intel only
```

#### Windows (MSVC / vcpkg)

```powershell
cmake .. -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build . --config Release
```

#### Windows (MSYS2 / MinGW)

```bash
cmake .. -G "MinGW Makefiles"
mingw32-make
```

Only the public `SMX_*` API functions are exported from the shared library. All internal symbols are hidden.

## Running tests

The project uses [doctest](https://github.com/doctest/doctest) for unit testing. Tests are not built by default.

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make
ctest
```

Or run the test binary directly for more detailed output:

```bash
./smx-tests
```

doctest is fetched automatically via CMake's FetchContent — no manual installation required.

### Integration tests

Integration tests require a physical SMX pad connected via USB and are built separately:

```bash
cmake .. -DBUILD_INTEGRATION_TESTS=ON
make smx-integration-tests
./smx-integration-tests
```

Tests skip gracefully if no hardware is detected. To record HID traffic during integration tests (for later replay-based regression testing):

```bash
SMX_CAPTURE_DIR=/tmp/captures ./smx-integration-tests
```

## Running the sample

After building, run the sample application:

```bash
# Linux / macOS
./smx-sample

# With custom polling rates (main thread ms, USB polling thread us)
./smx-sample 50 500

# Fire input callback on every packet (not just state changes)
./smx-sample --all-packets

# Enable panel pressure test mode (diagnostic LEDs)
./smx-sample --test-mode

# Combined
./smx-sample 50 500 --all-packets --test-mode

# Sensor test modes (prints values every 100ms)
./smx-sample --uncalibrated
./smx-sample --calibrated
./smx-sample --noise
./smx-sample --tare

# Windows (from build directory)
.\Release\smx-sample.exe   # vcpkg/MSVC
.\smx-sample.exe            # MSYS2/MinGW
```

The sample will scan for connected SMX pads and print connection events and panel input state changes to the console. Press `Ctrl+C` to quit.

Example output:

```
SMX SDK Multi Platform v0.2.1
Scanning for StepManiaX devices... Press Ctrl+C to quit.
Usage: ./smx-sample [main_thread_ms] [usb_polling_us] [--all-packets] [--test-mode]
       [--uncalibrated] [--calibrated] [--noise] [--tare]
Pad 0 connected (jumper: P1, serial: 1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d, fw: 5)
 0.052: Pad 0: input state 0000
 0.153: Pad 0: input state 0010
```

## API overview

```c
// Start scanning for devices. Callback fires on connect/disconnect/input change.
void SMX_Start(SMXUpdateCallback callback, void *pUser);

// Stop and disconnect.
void SMX_Stop();

// Optional: redirect log output.
void SMX_SetLogCallback(SMXLogCallback callback);

// Get connection status, serial number, firmware version.
void SMX_GetInfo(int pad, SMXInfo *info);

// Get current device configuration. Returns true if config is available.
bool SMX_GetConfig(int pad, SMXConfig *config);

// Write new configuration to device. Async; fires ConfigUpdated callback on completion.
void SMX_SetConfig(int pad, const SMXConfig *config);

// Get currently pressed panels as a bitmask.
uint16_t SMX_GetInputState(int pad);

// Assign serial numbers to controllers without one.
void SMX_SetSerialNumbers();

// Reset pad to factory default configuration.
void SMX_FactoryReset(int pad);

// Trigger immediate sensor recalibration.
void SMX_ForceRecalibration(int pad);

// Re-enable automatic panel lighting on both pads.
void SMX_ReenableAutoLights();

// Set panel LED colors (both pads, up to 30 FPS). lightDataSize must be 1350 or 864.
void SMX_SetLights2(const char *lightData, int lightDataSize);

// (Deprecated) Equivalent to SMX_SetLights2(lightData, 864).
void SMX_SetLights(const char lightData[864]);

// Set platform edge LED strip colors (88 LEDs × 3 bytes RGB = 264 bytes, firmware v4+).
void SMX_SetPlatformLights(const char *pLightData);

// Set panel-side diagnostic test mode.
void SMX_SetPanelTestMode(PanelTestMode mode);

// Set sensor test mode (read raw/calibrated sensor values).
void SMX_SetTestMode(int pad, SensorTestMode mode);

// Get most recent sensor test data. Returns true if data is available.
bool SMX_GetTestData(int pad, SMXSensorTestModeData *data);

// Configure thread sleep intervals (main thread ms, USB polling thread us).
void SMX_SetPollingRate(int iMainThreadMs, int iUSBPollingUs);

// Fire input state callback on every packet (true) or only on change (false, default).
void SMX_SetInputStateMode(bool bAlwaysFire);

// Get SDK version string.
const char *SMX_Version();

// Get elapsed time in seconds since SDK was initialized.
double SMX_GetMonotonicTime();

// Load a GIF as a panel animation. GIF must be 14x15 or 23x24.
bool SMX_LightsAnimation_Load(const char *gif, int size, int pad, SMX_LightsType type, const char **error);

// Enable/disable automatic GIF animation playback at 30 FPS.
void SMX_LightsAnimation_SetAuto(bool enable);

// Prepare a GIF animation for upload to firmware EEPROM. GIF must be 23x24, max 32 frames.
bool SMX_LightsUpload_PrepareUpload(const char *gif, int size, int pad, SMX_LightsType type, const char **error);

// Begin uploading prepared animation data. Callback reports progress 0-100.
void SMX_LightsUpload_BeginUpload(int pad, SMX_LightsUploadCallback callback, void *pUser);
```

`pad` is 0 for player 1, 1 for player 2.

## GIF animation format

`SMX_LightsAnimation_Load` accepts animated GIF files in a specific layout. The GIF encodes a 3×3 grid of panels with 1-pixel gutters between them.

`SMX_LightsUpload_PrepareUpload` accepts the same format but only supports 23×24 GIFs (25-LED mode) with a maximum of 32 frames per animation type. Each panel is limited to 15 unique colors.

### Supported dimensions

| GIF size | LED mode | Panel region | Notes |
|----------|----------|--------------|-------|
| 14×15 | 4×4 (16 LEDs) | 4×4 pixels per panel | Legacy mode |
| 23×24 | 5×5 (25 LEDs) | 8×8 pixels per panel | Full mode (firmware v4+) |

### Panel layout

Panels are arranged in a 3×3 grid, left-to-right, top-to-bottom:

```
┌────┬────┬────┐
│ 0  │ 1  │ 2  │
├────┼────┼────┤
│ 3  │ 4  │ 5  │
├────┼────┼────┤
│ 6  │ 7  │ 8  │
└────┴────┴────┘
```

**14×15 mode:** Each panel occupies a 4×4 pixel region at position `(col×5, row×5)`. The 1-pixel gaps between panels are ignored. The bottom row (y=14) is a flag row.

**23×24 mode:** Each panel occupies an 8×8 pixel region at position `(col×8, row×8)`. The outer 4×4 LED grid is sampled at even pixel coordinates `(dx×2, dy×2)`. The inner 3×3 LED grid is sampled at odd coordinates `(dx×2+1, dy×2+1)`. The bottom row (y=23) is a flag row.

### Loop frame marker

By default, the animation loops back to frame 0 when it reaches the end. To set a different loop point (e.g., for a pressed animation with a lead-in), set the bottom-left pixel (x=0, y=height-1) to white (R≥128, A=255) on the frame that should be the loop target. Only the first such marker is used.

### Frame timing

GIF frame delays are in 10ms units. The SDK snaps 30ms and 40ms delays to exactly 1/30s (33.3ms) to align with the panel update rate. Other delays are used as-is. Zero-delay frames default to 1/30s.

### Caveats

- **Color depth:** GIF is limited to 256 colors per frame (palette-based). For smooth gradients, use local palettes per frame.
- **Transparency:** Transparent pixels in the released animation show as black. Transparent pixels in the pressed animation leave the released animation visible underneath (overlay behavior).
- **Black pixels in pressed animations:** Fully black (0,0,0) pixels in a pressed animation are treated as transparent and won't overwrite the released animation. Use (1,0,0) if you need near-black.
- **Auto-lighting interaction:** While animations are playing via `SetAuto`, panels won't use their built-in step lighting. Call `SMX_LightsAnimation_SetAuto(false)` followed by `SMX_ReenableAutoLights()` to restore normal behavior.
- **Direct lights override:** Calling `SMX_SetLights2` directly pauses animation playback for ~100ms, then animation resumes. This allows applications to temporarily override animation without stopping it.

## Update callback reasons

The `SMXUpdateCallback` receives a `reason` bitmask indicating what triggered the callback. Multiple flags can be set simultaneously.

| Flag | Value | Triggered when |
|------|-------|----------------|
| `SMXUpdateCallback_Updated` | `1 << 0` | Always set on every callback. Use as a catch-all. |
| `SMXUpdateCallback_InputState` | `1 << 1` | Panel press/release state changed. By default, only fires when state actually changes. Call `SMX_SetInputStateMode(true)` to fire on every received input packet instead. Call `SMX_GetInputState()` for the current state. |
| `SMXUpdateCallback_Connected` | `1 << 2` | A pad has become fully connected (device info and config received). |
| `SMXUpdateCallback_Disconnected` | `1 << 3` | A pad has been disconnected. |
| `SMXUpdateCallback_ConfigUpdated` | `1 << 4` | Device configuration has been received or updated. |
| `SMXUpdateCallback_SensorTestData` | `1 << 5` | New sensor test data received. Call `SMX_GetTestData()` for the data. |

Use the `SMX_REASON_IS(reason, flag)` macro to check for specific flags:

```c
void MyCallback(int pad, int reason, void *pUser) {
    if (SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
        printf("Pad %d connected\n", pad);
    if (SMX_REASON_IS(reason, SMXUpdateCallback_InputState))
        printf("Pad %d input: %04x\n", pad, SMX_GetInputState(pad));
}
```

## SMXInfo fields

```c
struct SMXInfo {
    bool m_bConnected;        // true if pad is fully connected
    bool m_bIsPlayer2;        // physical player jumper setting on the PCB
    bool m_bHasSerialNumber;  // true if a serial number has been assigned
    char m_Serial[33];        // hex serial string (only valid if m_bHasSerialNumber)
    uint16_t m_iFirmwareVersion;
};
```

## Handling duplicate player jumpers

Each pad has a physical jumper on the PCB that sets it as P1 or P2. The SDK uses this to assign pads to slot 0 (P1) and slot 1 (P2). If both pads have the same jumper setting, the SDK can't determine which is which and will assign them to slots arbitrarily.

To detect this, check `m_bIsPlayer2` on both pads:

```c
SMXInfo info[2];
SMX_GetInfo(0, &info[0]);
SMX_GetInfo(1, &info[1]);

if(info[0].m_bConnected && info[1].m_bConnected &&
   info[0].m_bIsPlayer2 == info[1].m_bIsPlayer2)
{
    // Both pads have the same jumper setting.
    // Use serial numbers to tell them apart.
}
```

If pads don't have serial numbers assigned (`m_bHasSerialNumber` is false), call `SMX_SetSerialNumbers()` to assign them. Once assigned, serial numbers persist on the controller across power cycles and can be used to reliably identify specific pads regardless of jumper configuration.

## Panel bitmask

`SMX_GetInputState` returns a `uint16_t` where each bit corresponds to a panel:

```
Bit:    0   1   2   3   4   5   6   7   8
Panel: ┌───┬───┬───┐
       │ 0 │ 1 │ 2 │
       ├───┼───┼───┤
       │ 3 │ 4 │ 5 │
       ├───┼───┼───┤
       │ 6 │ 7 │ 8 │
       └───┴───┴───┘
```

## Project structure

```
├── include/
│   └── SMX.h                    # Public API header
├── src/
│   ├── SMX.cpp                  # Helpers, device logic, manager, API implementation
│   ├── SMXDeviceConnection.h    # HID I/O class (header)
│   ├── SMXDeviceConnection.cpp  # HID I/O class (implementation)
│   ├── SMXHIDInterface.h        # HID abstraction interfaces
│   ├── SMXHIDInterface.cpp      # Real hidapi-backed implementation
│   ├── SMXHIDRecorder.h         # HID traffic record/replay
│   ├── SMXHIDRecorder.cpp       # HID traffic record/replay implementation
│   ├── SMXConfigPacket.h        # Internal config struct
│   ├── SMXConfigPacket.cpp      # Old firmware config format conversion
│   ├── SMXPanelAnimation.cpp    # GIF animation loading and playback
│   ├── SMXVersion.h.in          # Version header template (configured by CMake)
│   └── vendor/
│       └── gif_load.h           # Single-header GIF decoder (public domain)
├── tests/
│   ├── test_main.cpp            # Version and log callback tests
│   ├── test_device_connection.cpp # Device connection tests with fake HID
│   ├── test_smx_manager.cpp     # Manager discovery and ordering tests
│   ├── test_config_packet.cpp   # Config format conversion tests
│   ├── test_config_api.cpp      # Config get/set API tests
│   ├── test_helpers.cpp         # Utility function tests
│   ├── test_helpers_manager.h   # Shared test infrastructure for manager-level tests
│   ├── test_move_semantics.cpp  # Move semantics / pad swap regression tests
│   ├── test_lights.cpp          # Panel LED control tests
│   ├── test_panel_animation.cpp # GIF animation loading tests
│   ├── test_replay.cpp          # HID traffic replay regression tests
│   └── test_integration.cpp     # Integration tests (real hardware)
├── sample/
│   └── sample.cpp               # Sample application
├── scripts/
│   └── decode_smxhid.py         # Decode .smxhid capture files for debugging
├── capture/                     # Recorded HID traffic for replay-based tests
├── bump-version.sh              # Version bump helper script
└── CMakeLists.txt               # Build configuration
```

## HID abstraction layer

All USB HID communication goes through two interfaces defined in `src/SMXHIDInterface.h`:

- `IHIDDevice` — represents an open connection to a single device (Read, Write, Close)
- `IHIDEnumerator` — handles device discovery and lifecycle (Init, Exit, Enumerate, Open)

The real implementation in `SMXHIDInterface.cpp` wraps hidapi. This is the only file that includes `<hidapi/hidapi.h>` or calls hidapi functions directly.

This abstraction exists for two reasons:

1. **Testability.** Tests inject a `FakeHIDDevice` that queues pre-built packets and captures writes, allowing full testing of packet parsing, state management, and connection logic without physical hardware.

2. **Replaceability.** If hidapi is ever swapped for a different HID library (or a platform-specific implementation), only `SMXHIDInterface.cpp` needs to change. The rest of the codebase is decoupled from the concrete HID library.

## Future projects

- **GIF animation editor** — A GUI tool for creating and previewing GIF animations that conform to the SMX panel format (correct dimensions, panel grid layout, color limits, loop frame markers). Would allow visual editing of per-panel animations with real-time preview on connected pads.

## Acknowledgments

This project is based on the [StepManiaX SDK](https://github.com/steprevolution/stepmaniax-sdk) by [Step Revolution LLC](https://stepmaniax.com). The original SDK is licensed under the MIT License.

## License

MIT — see [LICENSE.txt](LICENSE.txt).
