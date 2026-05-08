#ifndef SMX_H
#define SMX_H

#include <cstdint>
#include <cstring>

#ifdef SMX_EXPORTS
    #ifdef _WIN32
        #define SMX_API extern "C" __declspec(dllexport)
    #else
        #define SMX_API extern "C" __attribute__((visibility("default")))
    #endif
#else
    #ifdef _WIN32
        #define SMX_API extern "C" __declspec(dllimport)
    #else
        #define SMX_API extern "C"
    #endif
#endif

#define SERIAL_SIZE 16

struct SMXInfo;

/// Packed sensor settings for a single panel.
/// Contains low/high thresholds for load cell and FSR sensors,
/// as well as combined (multi-sensor) thresholds.
#pragma pack(push, 1)
struct packed_sensor_settings_t {
    uint8_t loadCellLowThreshold;
    uint8_t loadCellHighThreshold;
    uint8_t fsrLowThreshold[4];
    uint8_t fsrHighThreshold[4];
    uint16_t combinedLowThreshold;
    uint16_t combinedHighThreshold;
    uint16_t reserved;
};
#pragma pack(pop)

/// Configuration state for an SMX device.
/// Describes debounce settings, sensor thresholds, panel colors, auto-calibration
/// parameters, and other device settings. Total size is 250 bytes, tightly packed.
///
/// Read with SMX_GetConfig(). Write with SMX_SetConfig().
#pragma pack(push, 1)
struct SMXConfig
{
    uint8_t masterVersion = 0xFF;
    uint8_t configVersion = 0x05;
    uint8_t flags = 0;
    uint16_t debounceNodelayMilliseconds = 0;
    uint16_t debounceDelayMilliseconds = 0;
    uint16_t panelDebounceMicroseconds = 4000;
    uint8_t autoCalibrationMaxDeviation = 100;
    uint8_t badSensorMinimumDelaySeconds = 15;
    uint16_t autoCalibrationAveragesPerUpdate = 60;
    uint16_t autoCalibrationSamplesPerAverage = 500;
    uint16_t autoCalibrationMaxTare = 0xFFFF;
    uint8_t enabledSensors[5]{};
    uint8_t autoLightsTimeout = 1000/128;
    uint8_t stepColor[3*9]{};
    uint8_t platformStripColor[3]{};
    uint16_t autoLightPanelMask = 0xFFFF;
    uint8_t panelRotation{};
    packed_sensor_settings_t panelSettings[9]{};
    uint8_t preDetailsDelayMilliseconds = 5;
    uint8_t padding[49]{};
};
#pragma pack(pop)

// All functions are nonblocking. Getters return the most recent state.
// Setters return immediately and do their work in the background.

/// Reason codes for device state change callbacks.
/// When a device is connected, disconnected, or its input state changes,
/// this reason is passed to the update callback to indicate what happened.
/// Multiple reasons can be combined using bitwise OR to handle multiple state changes.
enum SMXUpdateCallbackReason : uint32_t {
    /// Always set on every callback firing. Applications can use this as a catch-all.
    SMXUpdateCallback_Updated = 1 << 0,

    /// Input state (pressed panels) has changed.
    /// Only fired when the state actually changes, not on every received input packet.
    /// When this is fired, SMX_GetInputState() will return the new state.
    SMXUpdateCallback_InputState = 1 << 1,

    /// Device has become fully connected (device info and config received).
    SMXUpdateCallback_Connected = 1 << 2,

    /// Device has been disconnected.
    SMXUpdateCallback_Disconnected = 1 << 3,

    /// Device configuration has been received or updated.
    SMXUpdateCallback_ConfigUpdated = 1 << 4,
};

// Helper macro for checking if a reason includes a specific flag
#define SMX_REASON_IS(reason, flag) (((reason) & (flag)) != 0)

/// Callback function type for device state changes.
/// Called asynchronously from the I/O thread when a device is connected, disconnected,
/// or its state changes (input, configuration, etc.).
///
/// IMPORTANT: This callback should return quickly to avoid stalling the I/O thread.
/// Do not call SMX_Stop() from within this callback. Long-running operations should be
/// queued for processing on the main application thread.
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param reason Reason for the callback (SMXUpdateCallback_Updated, SMXUpdateCallback_InputState, or combination).
///               Use SMX_REASON_IS(reason, SMXUpdateCallback_InputState) to check for input changes.
/// @param pUser Application context pointer passed to SMX_Start().
typedef void SMXUpdateCallback(int pad, SMXUpdateCallbackReason reason, void *pUser);

/// Initializes the SMX SDK and starts searching for connected devices.
/// Must be called once before using any other SDK functions.
/// The background I/O thread will automatically discover connected devices and
/// invoke the update callback when their state changes.
///
/// @warning The callback may be invoked from different background threads (the USB
/// polling thread for input state changes, and the main I/O thread for connection
/// and config events). Invocations are serialized internally so the callback will
/// never be called from two threads simultaneously, but it will not necessarily be
/// called from the application's main thread.
///
/// @param callback Function to be called asynchronously when devices are connected,
///                  disconnected, or their input state changes.
/// @param pUser Application-defined pointer passed to all callbacks for context.
SMX_API void SMX_Start(SMXUpdateCallback callback, void *pUser);

/// Shuts down the SMX SDK and disconnects from all devices.
/// Stops the background I/O thread, closes all device connections, and cleans up resources.
/// This function waits for the I/O thread to finish, so it may block briefly.
///
/// IMPORTANT: This must not be called from within an update callback.
/// If you need to shut down in response to a device event, queue it for later.
SMX_API void SMX_Stop();

typedef void SMXLogCallback(const char *log);

/// Sets a custom callback function to receive diagnostic log messages.
/// If not set, log messages are printed to stdout with timestamps.
/// This can be called before SMX_Start to capture initialization logs.
///
/// The log callback is invoked from both the main thread and the background I/O thread,
/// so it should be thread-safe if logging to shared resources.
///
/// @param callback Function that receives log strings. Pass nullptr to disable custom logging.
SMX_API void SMX_SetLogCallback(SMXLogCallback callback);

/// Queries information about a connected device.
/// Use this to detect which pads are connected and retrieve their properties
/// (serial number, firmware version, player number).
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param info [out] Pointer to SMXInfo structure to be filled with device info.
///            If pad is invalid or device is not connected, all fields are zeroed.
SMX_API void SMX_GetInfo(int pad, SMXInfo *info);

/// Retrieves the current configuration for a device.
/// Returns true if the config was successfully retrieved (device is connected and
/// config has been read). Returns false if the device is not connected.
///
/// If SMX_SetConfig was called but the write hasn't completed yet, this returns
/// the pending config (optimistic read).
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param config [out] Pointer to SMXConfig structure to be filled.
/// @return True if config was retrieved, false if device is not connected.
SMX_API bool SMX_GetConfig(int pad, SMXConfig *config);

/// Writes a new configuration to a device.
/// The write is asynchronous; the SMXUpdateCallback_ConfigUpdated callback will fire
/// when the device acknowledges the new configuration.
///
/// Config writes are rate-limited to once per second to prevent excess EEPROM wear.
/// If called more frequently, only the most recent config is sent after the cooldown.
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param config Pointer to the SMXConfig to write.
SMX_API void SMX_SetConfig(int pad, const SMXConfig *config);

/// Retrieves the current input state (pressed panels) for a device.
/// The returned value is a 16-bit bitmask where each bit corresponds to a panel.
/// Bit positions correspond to the 16 panels on an SMX device.
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @return 16-bit input state bitmask. Returns 0 if the device is not connected.
SMX_API uint16_t SMX_GetInputState(int pad);

/// Assigns random serial numbers to all connected devices that don't have one.
/// The serial numbers are permanently written to the device's non-volatile memory
/// and identify the physical controller.
/// This is an asynchronous operation; the actual programming happens in the
/// background I/O thread.
///
/// Note: Devices without a serial number show a hex string of all zeros or all F's.
SMX_API void SMX_SetSerialNumbers();

/// Resets a pad to its factory default configuration.
/// This sends a reset command to the device and re-reads the resulting configuration.
/// The operation is asynchronous; the SMXUpdateCallback_ConfigUpdated callback will fire
/// when the new configuration has been read back from the device.
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
SMX_API void SMX_FactoryReset(int pad);

/// Requests an immediate sensor recalibration on the specified pad.
/// This is normally not necessary, but can be helpful for diagnostics.
/// The operation is asynchronous and completes in the background.
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
SMX_API void SMX_ForceRecalibration(int pad);

/// Re-enables automatic panel lighting on both pads.
/// By default, panels light automatically when stepped on. If an application sends
/// lighting commands, auto-lighting is disabled. Call this to immediately re-enable it
/// without waiting for the timeout period to elapse.
SMX_API void SMX_ReenableAutoLights();

/// Panel-side diagnostic test modes.
/// These activate debug lighting on the panels and don't return data to the host.
/// Lights cannot be updated while a panel test mode is active.
enum PanelTestMode {
    PanelTestMode_Off = '0',
    PanelTestMode_PressureTest = '1',
};

/// Sets a panel test mode on all connected pads.
/// When enabled, the SDK periodically resends the command to keep the mode active
/// (the device times out after ~1 second without a refresh).
/// Lights cannot be updated while a panel test mode is active.
///
/// @param mode The test mode to activate, or PanelTestMode_Off to disable.
SMX_API void SMX_SetPanelTestMode(PanelTestMode mode);

/// Configures the polling rates for the SDK's background threads.
/// Can be called at any time after SMX_Start().
///
/// @param iMainThreadMs Sleep time in milliseconds for the main I/O thread (default: 50).
///                      Controls how often device connections and command responses are processed.
///                      Values above ~100ms may delay device connection handshakes, causing
///                      devices to appear uninitialized or missing serial numbers.
/// @param iUSBPollingUs Sleep time in microseconds for the USB polling thread (default: 1000).
///                      Controls input state latency. Lower values = lower latency but more CPU.
SMX_API void SMX_SetPollingRate(int iMainThreadMs, int iUSBPollingUs);

/// Controls when the SMXUpdateCallback_InputState callback fires.
/// By default (bAlwaysFire = false), the callback only fires when the input state
/// actually changes. When set to true, the callback fires on every received Report 3
/// packet, even if the state is unchanged from the previous packet.
///
/// @param bAlwaysFire If true, fire the input state callback on every Report 3 packet.
///                    If false (default), only fire when the state changes.
SMX_API void SMX_SetInputStateMode(bool bAlwaysFire);

/// Returns the SDK version string.
/// @return C-string containing the version (e.g., "0.1.1").
SMX_API const char *SMX_Version();

/// Returns the elapsed time in seconds since the SDK was initialized.
/// This is useful for logging timestamps and measuring elapsed time.
/// Uses a high-resolution monotonic clock that is not affected by system clock adjustments.
///
/// @return Elapsed time in seconds as a double (e.g., 1.234 for 1.234 seconds).
SMX_API double SMX_GetMonotonicTime();

/// Information about a connected SMX device.
/// This structure holds the current connection state and device metadata.
/// Query it with SMX_GetInfo() to detect devices and retrieve their properties.
struct SMXInfo
{
    /// True if we're fully connected to this controller.
    /// A device is fully connected when it has a valid HID connection,
    /// device info has been retrieved, and configuration is available.
    bool m_bConnected = false;

    /// True if the physical player jumper is set to player 2 mode.
    /// Player 1 device is typically placed in slot 0, Player 2 in slot 1,
    /// but the SDK automatically reorders devices to maintain this convention.
    bool m_bIsPlayer2 = false;

    /// True if this controller has been assigned a serial number.
    /// If false, call SMX_SetSerialNumbers() to assign one.
    /// Once assigned, the serial number is permanently stored in the device.
    bool m_bHasSerialNumber = false;

    /// Device serial number (null-terminated hex string, 32 chars + null).
    /// Format: 32 lowercase hexadecimal characters representing 16 bytes of device serial.
    /// Only meaningful if m_bHasSerialNumber is true.
    char m_Serial[33] = {};

    /// Device firmware version number.
    /// Used internally to determine protocol compatibility.
    uint16_t m_iFirmwareVersion = 0;
};

#endif
