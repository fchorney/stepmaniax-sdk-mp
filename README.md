# StepManiaX SDK Multi Platform

A cross-platform SDK for StepManiaX dance pads, fully re-implemented from the ground up for efficiency, low latency, and portability. This is a library only; it does not include a configuration tool or GUI. Applications and tools can be built on top of it.

## Table of Contents

- [Features](#features)
- [Feature Status](#feature-status)
- [Threading Architecture](#threading-architecture)
- [Dependencies](#dependencies)
- [Building](#building)
- [Running Tests](#running-tests)
- [Running the Sample](#running-the-sample)
- [API Overview](#api-overview)
- [Usage Examples](#usage-examples)
- [Update Callback Reasons](#update-callback-reasons)
- [SMXInfo Fields](#smxinfo-fields)
- [Handling Duplicate Player Jumpers](#handling-duplicate-player-jumpers)
- [Panel Bitmask](#panel-bitmask)
- [GIF Animation Format](#gif-animation-format)
- [Project Structure](#project-structure)
- [HID Abstraction Layer](#hid-abstraction-layer)
- [Documentation](#documentation)
- [Contributing](#contributing)
- [Reporting Issues](#reporting-issues)
- [Projects Using This Library](#projects-using-this-library)
- [Acknowledgments](#acknowledgments)
- [License](#license)

## Features

- Auto-discovers up to 2 connected SMX pads via USB HID
- Handles connect, disconnect, and reconnect
- Identifies P1 vs P2 and auto-corrects pad ordering
- Reads panel press/release state as a low-latency bitmask
- Full device configuration (thresholds, lighting, calibration)
- Sensor diagnostics (raw/calibrated values, noise, tare)
- Panel LED control (up to 30 FPS, 25 LEDs per panel)
- Platform LED strip control (44 LEDs per pad)
- GIF animation loading and automatic playback
- Animation upload to firmware EEPROM for offline playback
- Configurable polling rates for latency vs CPU tradeoff
- Cross-platform: Linux, macOS (Intel and Apple Silicon), Windows

## Feature Status

All features from the original StepManiaX SDK have been implemented, plus several new ones.

### Original SDK Features

| Feature | API | Notes |
|---------|-----|-------|
| Device discovery and connection | `SMX_Start`, `SMX_Stop` | Auto-discovers up to 2 pads via USB HID |
| Log callback | `SMX_SetLogCallback` | Redirect SDK log output to your own handler |
| Device info | `SMX_GetInfo` | Connection status, serial, firmware version, player ID |
| Input state | `SMX_GetInputState` | Panel press/release as a 16-bit bitmask |
| Serial number assignment | `SMX_SetSerialNumbers` | Writes random serial to devices without one |
| SDK version | `SMX_Version` | Returns version string (e.g. "1.0.0") |
| Factory reset | `SMX_FactoryReset` | Reset pad to default configuration |
| Force recalibration | `SMX_ForceRecalibration` | Trigger immediate sensor recalibration |
| Re-enable auto lights | `SMX_ReenableAutoLights` | Return panels to automatic step lighting |
| Panel test mode | `SMX_SetPanelTestMode` | Panel-side diagnostic lighting (pressure test) |
| Get/set configuration | `SMX_GetConfig`, `SMX_SetConfig` | Read/write thresholds, lighting, sensor settings |
| Sensor test mode | `SMX_SetTestMode`, `SMX_GetTestData` | Read raw/calibrated sensor values for diagnostics |
| Panel LED control | `SMX_SetLights2` | Set RGB colors for all panel LEDs (up to 30 FPS) |
| Platform LED strip | `SMX_SetPlatformLights` | Control platform edge LED strip (firmware v4+) |
| GIF animation playback | `SMX_LightsAnimation_Load`, `SMX_LightsAnimation_SetAuto` | Load and auto-play GIF animations on panels |
| Animation upload | `SMX_LightsUpload_PrepareUpload`, `SMX_LightsUpload_BeginUpload` | Upload animations to firmware EEPROM |

### New Features (not in original SDK)

| Feature | API | Notes |
|---------|-----|-------|
| Polling rate configuration | `SMX_SetPollingRate` | Tune latency vs CPU usage per-thread |
| Input state callback mode | `SMX_SetInputStateMode` | Fire callback on every packet or only on state change |
| Monotonic time | `SMX_GetMonotonicTime` | High-resolution elapsed time for timestamps |

## Threading Architecture

This SDK uses a two-thread design that differs from the original StepManiaX SDK. The original uses a single I/O thread for all USB communication. This version separates input state polling into its own dedicated thread to minimize input latency.

- **USB polling thread** - continuously reads HID data from both pads. Input state packets (Report 3) are parsed inline and update an atomic variable immediately, ensuring the lowest possible latency for panel press/release detection. Non-input packets (Report 6) are buffered for the main thread.
- **Main I/O thread** - handles device discovery, connection management, configuration, and command processing. Wakes on Report 6 data from the USB polling thread or on a configurable timeout.

Both thread sleep intervals are configurable via `SMX_SetPollingRate(int mainThreadMs, int usbPollingUs)`. The USB polling thread defaults to 1000us between cycles; the main thread defaults to 50ms.

### Device report rate

The USB polling thread reads as fast as the device sends data, but the actual input report rate is determined by the pad's firmware, not the SDK's polling interval. Based on observation (the pad firmware is not open source):

- **Idle:** ~10 Report 3 packets/sec (likely a periodic heartbeat)
- **Active input:** ~50 packets/sec during panel presses (one report per state transition)

The pad only sends input reports when the panel state changes or on a low-frequency heartbeat, rather than continuously streaming at the USB endpoint's maximum rate. The SDK's polling interval only needs to be fast enough to not miss packets between reads.

## Dependencies

- **CMake** 3.14+
- **C++14** compiler
- **[hidapi](https://github.com/libusb/hidapi)** - lightweight HID library
- **[gif_load](https://github.com/hidefromkgb/gif_load)** - single-header animated GIF decoder (vendored, public domain)

### Vendored libraries

The `src/vendor/` directory contains single-header libraries included directly in the source tree:

| Library | File | License | Purpose |
|---------|------|---------|---------|
| gif_load | `src/vendor/gif_load.h` | Public domain (Unlicense) | Animated GIF decoding for panel animations |

## Original SDK source

The original StepManiaX SDK is included as a git submodule at `original_sdk/` for reference. To initialize it:

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

### macOS (Intel and Apple Silicon)

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

This works on both Intel and Apple Silicon Macs. Homebrew installs native arm64 libraries on Apple Silicon and x86_64 on Intel. No special flags are needed.

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

### Build options

| Option | Default | Description |
|--------|---------|-------------|
| `BUILD_SHARED_LIBS` | `ON` | Build shared library (set to `OFF` for static) |
| `BUILD_SAMPLE` | `OFF` | Build the sample application |
| `BUILD_TESTS` | `OFF` | Build unit tests |
| `BUILD_INTEGRATION_TESTS` | `OFF` | Build integration tests (require real hardware) |
| `ENABLE_COVERAGE` | `OFF` | Build with code coverage instrumentation (gcov/lcov) |
| `ENABLE_ASAN` | `OFF` | Build with AddressSanitizer (memory error detection) |
| `ENABLE_TSAN` | `OFF` | Build with ThreadSanitizer (data race detection) |

```bash
cmake .. -DBUILD_SAMPLE=ON                          # shared lib + sample
cmake .. -DBUILD_SHARED_LIBS=OFF                    # static lib only
cmake .. -DBUILD_SHARED_LIBS=OFF -DBUILD_SAMPLE=ON  # static lib + sample
```

Only the public `SMX_*` API functions are exported from the shared library. All internal symbols are hidden.

## Running Tests

```bash
mkdir build && cd build
cmake .. -DBUILD_TESTS=ON
make
ctest
```

Or run the test binary directly for detailed output:

```bash
./smx-tests
```

[doctest](https://github.com/doctest/doctest) is fetched automatically via CMake's FetchContent.

### Integration tests

Integration tests require a physical SMX pad connected via USB:

```bash
cmake .. -DBUILD_INTEGRATION_TESTS=ON
make smx-integration-tests
./smx-integration-tests
```

Tests skip gracefully if no hardware is detected. To record HID traffic for replay-based regression testing:

```bash
SMX_CAPTURE_DIR=/tmp/captures ./smx-integration-tests
```

### Code coverage

Build with coverage instrumentation to see which lines are exercised by tests:

```bash
# Install lcov if needed:
# Debian/Ubuntu: sudo apt install lcov
# Fedora: sudo dnf install lcov
# macOS: brew install lcov
# Arch: sudo pacman -S lcov

mkdir build && cd build
cmake .. -DBUILD_TESTS=ON -DENABLE_COVERAGE=ON
make
./smx-tests
lcov --capture --directory . --output-file coverage_raw.info
lcov --extract coverage_raw.info '*/src/*' --output-file coverage.info
genhtml coverage.info --output-directory coverage_report
```

Open `coverage_report/index.html` in a browser to view per-file and per-line coverage.

### Sanitizers

AddressSanitizer detects memory errors (buffer overflows, use-after-free, leaks):

```bash
cmake .. -DBUILD_TESTS=ON -DENABLE_ASAN=ON
make
./smx-tests
```

ThreadSanitizer detects data races between threads:

```bash
cmake .. -DBUILD_TESTS=ON -DENABLE_TSAN=ON
make
./smx-tests
```

Both sanitizers print diagnostics to stderr at runtime if they detect issues. A clean run produces no extra output. ASan and TSan cannot be used simultaneously.

## Running the Sample

```bash
cd build
cmake .. -DBUILD_SAMPLE=ON
make
./smx-sample
```

The sample scans for connected SMX pads and prints connection events and panel input state changes. Press `Ctrl+C` to quit.

## API Overview

All functions are nonblocking. Getters return the most recent state. Setters return immediately and do their work in the background. `pad` is 0 for Player 1, 1 for Player 2.

```c
// Lifecycle
void SMX_Start(SMXUpdateCallback callback, void *pUser);
void SMX_Stop();
void SMX_SetLogCallback(SMXLogCallback callback);
const char *SMX_Version();
double SMX_GetMonotonicTime();

// Device info and state
void SMX_GetInfo(int pad, SMXInfo *info);
uint16_t SMX_GetInputState(int pad);
bool SMX_GetConfig(int pad, SMXConfig *config);
void SMX_SetConfig(int pad, const SMXConfig *config);

// Device commands
void SMX_SetSerialNumbers();
void SMX_FactoryReset(int pad);
void SMX_ForceRecalibration(int pad);
void SMX_ReenableAutoLights();

// Lighting
void SMX_SetLights2(const char *lightData, int lightDataSize);
void SMX_SetLights(const char lightData[864]);  // deprecated, use SetLights2
void SMX_SetPlatformLights(const char *pLightData);

// Diagnostics
void SMX_SetPanelTestMode(PanelTestMode mode);
void SMX_SetTestMode(int pad, SensorTestMode mode);
bool SMX_GetTestData(int pad, SMXSensorTestModeData *data);

// Configuration
void SMX_SetPollingRate(int iMainThreadMs, int iUSBPollingUs);
void SMX_SetInputStateMode(bool bAlwaysFire);

// Animations
bool SMX_LightsAnimation_Load(const char *gif, int size, int pad, SMX_LightsType type, const char **error);
void SMX_LightsAnimation_SetAuto(bool enable);
bool SMX_LightsUpload_PrepareUpload(const char *gif, int size, int pad, SMX_LightsType type, const char **error);
void SMX_LightsUpload_BeginUpload(int pad, SMX_LightsUploadCallback callback, void *pUser);
```

## Usage Examples

### Basic initialization and input polling

```c
#include "SMX.h"
#include <stdio.h>

void OnUpdate(int pad, SMXUpdateCallbackReason reason, void *pUser) {
    if (SMX_REASON_IS(reason, SMXUpdateCallback_Connected)) {
        SMXInfo info;
        SMX_GetInfo(pad, &info);
        printf("Pad %d connected (fw %d)\n", pad, info.m_iFirmwareVersion);
    }
    if (SMX_REASON_IS(reason, SMXUpdateCallback_InputState)) {
        printf("Pad %d state: %04x\n", pad, SMX_GetInputState(pad));
    }
}

int main() {
    SMX_Start(OnUpdate, NULL);
    // ... run your application ...
    SMX_Stop();
}
```

### Setting panel LED colors

```c
// 25-LED mode: 2 pads x 9 panels x 25 LEDs x 3 RGB = 1350 bytes
char lights[1350] = {0};

// Set panel 4 (center) on pad 0 to solid red (outer 4x4 grid)
// Each panel has 16 outer LEDs (4x4) then 9 inner LEDs (3x3)
int panelOffset = 4 * 25 * 3;  // panel 4, 25 LEDs per panel, 3 bytes per LED
for (int led = 0; led < 16; led++) {
    lights[panelOffset + led * 3 + 0] = 255;  // R
    lights[panelOffset + led * 3 + 1] = 0;    // G
    lights[panelOffset + led * 3 + 2] = 0;    // B
}

SMX_SetLights2(lights, 1350);
```

### Reading and modifying configuration

```c
SMXConfig config;
if (SMX_GetConfig(0, &config)) {
    // Adjust panel 4 (center) FSR threshold
    config.panelSettings[4].fsrLowThreshold[0] = 20;
    config.panelSettings[4].fsrHighThreshold[0] = 15;
    SMX_SetConfig(0, &config);
}
```

### Loading a GIF animation

```c
// Read GIF file into memory
FILE *f = fopen("idle_animation.gif", "rb");
fseek(f, 0, SEEK_END);
int size = ftell(f);
fseek(f, 0, SEEK_SET);
char *gif = malloc(size);
fread(gif, 1, size, f);
fclose(f);

// Load as the "released" (idle) animation for pad 0
const char *error = NULL;
if (!SMX_LightsAnimation_Load(gif, size, 0, SMX_LightsType_Released, &error))
    printf("Error: %s\n", error);

free(gif);

// Start automatic playback at 30 FPS
SMX_LightsAnimation_SetAuto(true);
```

### Uploading animations to firmware

Loading a GIF with `SMX_LightsAnimation_Load` plays it from the host at 30 FPS (the SDK sends LED data every frame). Uploading with `SMX_LightsUpload_PrepareUpload` + `SMX_LightsUpload_BeginUpload` writes the animation to the pad's EEPROM so it plays autonomously without a host connection.

```c
// Prepare both released and pressed animations for pad 0
const char *error = NULL;
if (!SMX_LightsUpload_PrepareUpload(releasedGif, releasedSize, 0, SMX_LightsType_Released, &error))
    printf("Error: %s\n", error);
if (!SMX_LightsUpload_PrepareUpload(pressedGif, pressedSize, 0, SMX_LightsType_Pressed, &error))
    printf("Error: %s\n", error);

// Begin the upload (progress callback reports 0-100)
SMX_LightsUpload_BeginUpload(0, [](int progress, void *pUser) {
    printf("Upload progress: %d%%\n", progress);
    if (progress == 100)
        printf("Upload complete!\n");
}, NULL);
```

Upload constraints: GIF must be 23x24 (25-LED mode), max 32 frames, max 15 unique colors per panel. The upload writes to EEPROM and persists across power cycles.

### Sensor diagnostics

```c
// Enable calibrated sensor reading mode on pad 0
SMX_SetTestMode(0, SensorTestMode_CalibratedValues);

// In your callback or polling loop:
SMXSensorTestModeData data;
if (SMX_GetTestData(0, &data)) {
    for (int panel = 0; panel < 9; panel++) {
        if (!data.bHaveDataFromPanel[panel]) continue;
        printf("Panel %d sensors: %d %d %d %d\n", panel,
            data.sensorLevel[panel][0], data.sensorLevel[panel][1],
            data.sensorLevel[panel][2], data.sensorLevel[panel][3]);
    }
}

// Disable when done
SMX_SetTestMode(0, SensorTestMode_Off);
```

### Platform LED strip

```c
// 88 LEDs total (44 per pad) x 3 RGB = 264 bytes
char stripData[264] = {0};

// Set first 10 LEDs on pad 0 to blue
for (int i = 0; i < 10; i++) {
    stripData[i * 3 + 2] = 255;  // B
}

SMX_SetPlatformLights(stripData);
```

**Note:** The platform LED strips are not designed for animation. They are intended to be set to a static color. Rapidly updating them (e.g. at 30 FPS) may cause visual artifacts or unreliable behavior.

## Update Callback Reasons

The `SMXUpdateCallback` receives a `reason` bitmask. Multiple flags can be set simultaneously.

| Flag | Value | Triggered when |
|------|-------|----------------|
| `SMXUpdateCallback_Updated` | `1 << 0` | Always set on every callback |
| `SMXUpdateCallback_InputState` | `1 << 1` | Panel press/release state changed |
| `SMXUpdateCallback_Connected` | `1 << 2` | Pad fully connected (device info and config received) |
| `SMXUpdateCallback_Disconnected` | `1 << 3` | Pad disconnected |
| `SMXUpdateCallback_ConfigUpdated` | `1 << 4` | Configuration received or updated |
| `SMXUpdateCallback_SensorTestData` | `1 << 5` | New sensor test data available |

Use `SMX_REASON_IS(reason, flag)` to check for specific flags:

```c
if (SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
    printf("Pad %d connected\n", pad);
```

## SMXInfo Fields

```c
struct SMXInfo {
    bool m_bConnected;          // true if pad is fully connected
    bool m_bIsPlayer2;          // physical player jumper setting on the PCB
    bool m_bHasSerialNumber;    // true if a serial number has been assigned
    char m_Serial[33];          // hex serial string (32 chars + null)
    uint16_t m_iFirmwareVersion;
};
```

## Handling Duplicate Player Jumpers

Each pad has a physical jumper on the PCB that sets it as P1 or P2. The SDK uses this to assign pads to slot 0 (P1) and slot 1 (P2). If both pads have the same jumper setting, the SDK assigns them arbitrarily.

```c
SMXInfo info[2];
SMX_GetInfo(0, &info[0]);
SMX_GetInfo(1, &info[1]);

if (info[0].m_bConnected && info[1].m_bConnected &&
    info[0].m_bIsPlayer2 == info[1].m_bIsPlayer2) {
    // Both pads have the same jumper setting.
    // Use serial numbers to tell them apart.
}
```

If pads don't have serial numbers (`m_bHasSerialNumber` is false), call `SMX_SetSerialNumbers()` to assign them. Once assigned, serial numbers persist across power cycles.

## Panel Bitmask

`SMX_GetInputState` returns a `uint16_t` where each bit corresponds to a panel:

```
Bit:    0   1   2   3   4   5   6   7   8   9-15
Panel: +---+---+---+
       | 0 | 1 | 2 |  (top row)
       +---+---+---+
       | 3 | 4 | 5 |  (middle row)
       +---+---+---+
       | 6 | 7 | 8 |  (bottom row)
       +---+---+---+
       Bits 9-15: unused (always 0)
```

## GIF Animation Format

`SMX_LightsAnimation_Load` accepts animated GIF files encoding a 3x3 grid of panels with 1-pixel gutters.

### Supported dimensions

| GIF size | LED mode | Panel region | Notes |
|----------|----------|--------------|-------|
| 14x15 | 4x4 (16 LEDs) | 4x4 pixels per panel | Legacy mode |
| 23x24 | 5x5 (25 LEDs) | 8x8 pixels per panel | Full mode (firmware v4+) |

### Panel layout in the GIF

```
+----+----+----+
| 0  | 1  | 2  |
+----+----+----+
| 3  | 4  | 5  |
+----+----+----+
| 6  | 7  | 8  |
+----+----+----+
```

**14x15 mode:** Each panel is a 4x4 pixel region at `(col*5, row*5)`. Gaps are ignored. Bottom row (y=14) is a flag row.

**23x24 mode:** Each panel is an 8x8 pixel region at `(col*8, row*8)`. Outer 4x4 grid sampled at even coordinates. Inner 3x3 grid sampled at odd coordinates. Bottom row (y=23) is a flag row.

### Loop frame marker

To set a loop point, set the bottom-left pixel (x=0, y=height-1) to white (R>=128, A=255) on the target frame. Only the first marker is used. Default loops to frame 0.

### Upload constraints

`SMX_LightsUpload_PrepareUpload` only supports 23x24 GIFs with a maximum of 32 frames per animation type. Each panel is limited to 15 unique colors (black is treated as transparent).

## Project Structure

```
├── include/
│   └── SMX.h                    # Public API header
├── src/
│   ├── SMX.cpp                  # Public C API and test-only API
│   ├── SMXHelpers.h             # Internal utility function declarations
│   ├── SMXHelpers.cpp           # Logging, timing, formatting, binary conversion
│   ├── SMXDevice.h              # Per-controller device class (header)
│   ├── SMXDevice.cpp            # Per-controller device class (implementation)
│   ├── SMXManager.h             # Device manager / orchestration (header)
│   ├── SMXManager.cpp           # Device manager / orchestration (implementation)
│   ├── SMXDeviceConnection.h    # HID I/O class (header)
│   ├── SMXDeviceConnection.cpp  # HID I/O class (implementation)
│   ├── SMXProtocolConstants.h   # Device protocol constants (LEDs, timing, USB IDs)
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
├── tests/                       # Unit and integration tests
├── sample/sample.cpp            # Sample application
├── scripts/decode_smxhid.py     # Decode .smxhid capture files for debugging
├── capture/                     # Recorded HID traffic for replay-based tests
├── docs/                        # Additional documentation
└── CMakeLists.txt               # Build configuration
```

## HID Abstraction Layer

All USB HID communication goes through two interfaces defined in `src/SMXHIDInterface.h`:

- `IHIDDevice` - represents an open connection to a single device (Read, Write, Close)
- `IHIDEnumerator` - handles device discovery and lifecycle (Init, Exit, Enumerate, Open)

The real implementation in `SMXHIDInterface.cpp` wraps hidapi. This is the only file that includes `<hidapi/hidapi.h>` or calls hidapi functions directly.

This abstraction enables testing without physical hardware (via `FakeHIDDevice` and `ReplayHIDDevice`) and makes the HID backend swappable without touching the rest of the codebase.

## Documentation

Additional documentation is available in the `docs/` directory:

- [API Code Paths](docs/API_CODE_PATHS.md) - traces the execution path of each public API function through the SDK internals, including threading model diagrams and the device connection lifecycle
- [USB Protocol](docs/USB_PROTOCOL.md) - describes the USB HID communication protocol used by SMX pads, including packet formats, command reference, and hardware architecture

### Decoding HID captures

The `scripts/decode_smxhid.py` script decodes `.smxhid` capture files to human-readable output:

```bash
./scripts/decode_smxhid.py /tmp/captures/2026-05-12_11-10-56/device_0.smxhid

# Show only command/config packets (hide input state)
./scripts/decode_smxhid.py device_0.smxhid --commands

# Show only input state packets
./scripts/decode_smxhid.py device_0.smxhid --input

# Include raw packet data as byte arrays
./scripts/decode_smxhid.py device_0.smxhid --data
```

## Contributing

1. Fork the repository and create a branch prefixed with your initials (e.g. `fc/add-feature`).
2. Follow the code style conventions below.
3. Write tests for any new functionality or bug fixes.
4. Ensure `ctest` passes with `BUILD_TESTS=ON`.
5. Open a pull request with a clear description of what changed and why.

Keep PRs focused on a single concern. If you're fixing a bug and also refactoring nearby code, split them into separate PRs.

### Code style

- 4-space indentation, no tabs.
- Opening braces on the same line for control flow (`if`, `for`, `while`), next line for function/class definitions.
- Member variables use `m_` prefix with type-hinting: `m_b` (bool), `m_i` (int), `m_s` (string), `m_p` (pointer), `m_a` (array/vector).
- Local variables use camelCase with similar type prefixes (`sError`, `iCount`, `bActive`).
- Functions and methods use PascalCase (`GetInputState`, `SendCommand`).
- Constants use UPPER_SNAKE_CASE.
- `using namespace std;` is used within implementation files.
- Doxygen-style `///` comments for public/important functions with `@param`, `@return` tags.
- Prefer `const` parameters and `lock_guard` for mutex management.
- No trailing whitespace. Files end with a newline.

### Key considerations

- **Input latency is paramount.** Any change to the USB polling thread or Report 3 handling path must not add latency.
- **Thread safety.** The two-thread model requires careful attention to which thread owns which data.
- **Keep the public API minimal.** Only `SMX_*` functions are exported. Internal classes stay in the `SMX` namespace or anonymous namespaces.
- **Cross-platform.** All code must build on Linux, macOS, and Windows. Use standard C++14 and hidapi only.
- **Always write tests.** Every bug fix or new feature must include tests.

## Reporting Issues

When filing an issue, please include:

- **OS and version** (e.g. Ubuntu 24.04, macOS 15.1, Windows 11)
- **Compiler and version** (e.g. GCC 13.3, Clang 18, MSVC 2022)
- **hidapi version** (check with `pkg-config --modversion hidapi-hidraw` on Linux)
- **SMX pad firmware version** (printed by the SDK on connection, e.g. "Master version: 5")
- **Steps to reproduce** the issue
- **Expected vs actual behavior**
- **Relevant log output** (set a log callback or check stdout)

For hardware-related issues, recording a HID traffic capture is extremely helpful:

```bash
SMX_CAPTURE_DIR=/tmp/captures ./your-application
# Then attach the .smxhid files from /tmp/captures/ to the issue
```

Note: HID traffic recording is built into the shared library. Set the `SMX_CAPTURE_DIR` environment variable before launching any application using the SDK and it will automatically record all HID traffic to timestamped `.smxhid` files in that directory.

## Projects Using This Library

- **[StepManiaX GIF Maker](https://github.com/fchorney/stepmaniax-gif-maker)** - a cross-platform pixel editor for creating, editing, previewing, and uploading animated LED GIFs to StepManiaX dance pads

## Acknowledgments

This project is based on the [StepManiaX SDK](https://github.com/steprevolution/stepmaniax-sdk) by [Step Revolution LLC](https://stepmaniax.com). The original SDK is licensed under the MIT License.

## License

MIT. See [LICENSE.txt](LICENSE.txt).
