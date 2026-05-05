# StepManiaX SDK Multi Platform

A minimal, cross-platform SDK for StepManiaX dance pads. Supports device discovery, connection management, player identification, and reading panel input state.

## Features

- Auto-discovers up to 2 connected SMX pads via USB HID
- Handles connect, disconnect, and reconnect
- Identifies P1 vs P2 and auto-corrects pad ordering
- Reads panel press/release state as a bitmask
- Assigns serial numbers to controllers
- Reports firmware version
- Cross-platform: Linux, macOS (Intel & Apple Silicon), Windows

## Threading architecture

This SDK uses a two-thread design that differs from the original StepManiaX SDK. The original SDK uses a single I/O thread for all USB communication. This version separates input state polling into its own dedicated thread to minimize input latency.

- **USB polling thread** — continuously reads HID data from both pads. Input state packets (Report 3) are parsed inline and update an atomic variable immediately, ensuring the lowest possible latency for panel press/release detection. Non-input packets (Report 6) are buffered for the main thread.
- **Main I/O thread** — handles device discovery, connection management, configuration, and command processing. Wakes on Report 6 data from the USB polling thread or on a configurable timeout.

Both thread sleep intervals are configurable via `SMX_SetPollingRate(int mainThreadMs, int usbPollingUs)`. The USB polling thread defaults to 1000µs between cycles; the main thread defaults to 100ms.

## Dependencies

- **CMake** 3.14+
- **C++14** compiler
- **[hidapi](https://github.com/libusb/hidapi)** — lightweight HID library

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

```bash
cmake ..                                  # shared lib only (default)
cmake .. -DBUILD_SAMPLE=ON               # shared lib + sample
cmake .. -DBUILD_SHARED_LIBS=OFF         # static lib only
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

## Running the sample

After building, run the sample application:

```bash
# Linux / macOS
./smx-sample

# With custom polling rates (main thread ms, USB polling thread us)
./smx-sample 50 500

# Windows (from build directory)
.\Release\smx-sample.exe   # vcpkg/MSVC
.\smx-sample.exe            # MSYS2/MinGW
```

The sample will scan for connected SMX pads and print connection events and panel input state changes to the console. Press `Ctrl+C` to quit.

Example output:

```
SMX SDK Multi Platform v0.1.0
Scanning for StepManiaX devices... Press Ctrl+C to quit.
Usage: ./smx-sample [main_thread_ms] [usb_polling_us]
Pad 0 connected (jumper: P1, serial: 1a2b3c4d5e6f7a8b9c0d1e2f3a4b5c6d, fw: 5)
Pad 0: input state 0000
Pad 0: input state 0010
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

// Get currently pressed panels as a bitmask.
uint16_t SMX_GetInputState(int pad);

// Assign serial numbers to controllers without one.
void SMX_SetSerialNumbers();

// Configure thread sleep intervals (main thread ms, USB polling thread us).
void SMX_SetPollingRate(int iMainThreadMs, int iUSBPollingUs);

// Fire input state callback on every packet (true) or only on change (false, default).
void SMX_SetInputStateMode(bool bAlwaysFire);

// Get SDK version string.
const char *SMX_Version();
```

`pad` is 0 for player 1, 1 for player 2.

## Update callback reasons

The `SMXUpdateCallback` receives a `reason` bitmask indicating what triggered the callback. Multiple flags can be set simultaneously.

| Flag | Value | Triggered when |
|------|-------|----------------|
| `SMXUpdateCallback_Updated` | `1 << 0` | Always set on every callback. Use as a catch-all. |
| `SMXUpdateCallback_InputState` | `1 << 1` | Panel press/release state changed. By default, only fires when state actually changes. Call `SMX_SetInputStateMode(true)` to fire on every received input packet instead. Call `SMX_GetInputState()` for the current state. |
| `SMXUpdateCallback_Connected` | `1 << 2` | A pad has become fully connected (device info and config received). |
| `SMXUpdateCallback_Disconnected` | `1 << 3` | A pad has been disconnected. |
| `SMXUpdateCallback_ConfigUpdated` | `1 << 4` | Device configuration has been received or updated. |

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
│   ├── SMXConfigPacket.h        # Internal config struct
│   └── SMXConfigPacket.cpp      # Old firmware config format conversion
├── sample/
│   └── sample.cpp               # Sample application
└── CMakeLists.txt               # Build configuration
```

## Roadmap

This SDK currently implements a subset of the original StepManiaX SDK's functionality. Planned additions include:

- Reading per-panel sensor data
- Writing pad configuration (thresholds, lighting, etc.)
- Factory reset
- Panel test modes
- GIF Upload
- and More!

## Acknowledgments

This project is based on the [StepManiaX SDK](https://github.com/steprevolution/stepmaniax-sdk) by [Step Revolution LLC](https://stepmaniax.com). The original SDK is licensed under the MIT License.

## License

MIT — see [LICENSE.txt](LICENSE.txt).
