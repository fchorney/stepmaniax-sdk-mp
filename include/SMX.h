#ifndef SMX_H
#define SMX_H

#include <cstdint>
#include <cmath>

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

/// Bits for SMXConfig::flags (masterVersion >= 4).
enum SMXConfigFlags {
    // If set, panels will use the pressed animation when pressed, and stepColor
    // is ignored. If unset, panels will be lit solid using stepColor.
    PlatformFlags_AutoLightingUsePressedAnimations = 1 << 0,

    // If set, panels are using FSRs, otherwise load cells.
    PlatformFlags_FSR = 1 << 1,
};

/// Packed sensor settings for a single panel.
/// Contains low/high thresholds for load cell and FSR sensors,
/// as well as combined (multi-sensor) thresholds.
#pragma pack(push, 1)
struct packed_sensor_settings_t {
    // Load cell thresholds:
    uint8_t loadCellLowThreshold;
    uint8_t loadCellHighThreshold;

    // FSR (Force Sensitive Resistor) thresholds:
    uint8_t fsrLowThreshold[4];
    uint8_t fsrHighThreshold[4];

    uint16_t combinedLowThreshold;
    uint16_t combinedHighThreshold;

    // This must be left unchanged.
    uint16_t reserved;
};
#pragma pack(pop)

/// Configuration state for an SMX device.
/// The order and packing of this struct corresponds to the configuration packet sent to
/// the master controller, so it must not be changed.
///
/// Read with SMX_GetConfig(). Write with SMX_SetConfig().
#pragma pack(push, 1)
struct SMXConfig
{
    // The firmware version of the master controller. Where supported (version 2+), this
    // will always read back the firmware version. Defaults to 0xFF on version 1.
    // We always write 0xFF here so it doesn't change on that firmware version.
    uint8_t masterVersion = 0xFF;

    // The version of this config packet. This can be used by the firmware to know which
    // values have been filled in. Unrelated to firmware version.
    // Versions: 0xFF=pre-versioning, 0x00=first, 0x02=per-panel thresholds,
    //           0x03=debounceDelayMs added, 0x05=current (packed_sensor_settings_t)
    uint8_t configVersion = 0x05;

    // Packed flags (masterVersion >= 4).
    uint8_t flags = 0;

    // These are internal tunables and should be left unchanged.
    uint16_t debounceNodelayMilliseconds = 0;
    uint16_t debounceDelayMilliseconds = 0;
    uint16_t panelDebounceMicroseconds = 4000;
    uint8_t autoCalibrationMaxDeviation = 100;
    uint8_t badSensorMinimumDelaySeconds = 15;
    uint16_t autoCalibrationAveragesPerUpdate = 60;
    uint16_t autoCalibrationSamplesPerAverage = 500;

    // The maximum tare value to calibrate to (except on startup).
    uint16_t autoCalibrationMaxTare = 0xFFFF;

    // Which sensors on each panel to enable. Packed with four sensors on two pads per byte:
    // enabledSensors[0] & 1 is the first sensor on the first pad, and so on.
    uint8_t enabledSensors[5]{};

    // How long the master controller will wait for a lights command before resuming
    // auto-lights. In 128ms units.
    uint8_t autoLightsTimeout = 1000/128;

    // The color to use for each panel when auto-lighting in master mode.
    // These colors should be scaled to the 0-170 range.
    uint8_t stepColor[3*9]{};

    // The default color to set the platform LED strip to.
    uint8_t platformStripColor[3]{};

    // Which panels to enable auto-lighting for. Disabled panels will be unlit.
    // 0x01 = panel 0, 0x02 = panel 1, 0x04 = panel 2, etc.
    uint16_t autoLightPanelMask = 0xFFFF;

    // The rotation of the panel (0-3, 90° increments). Currently unused.
    uint8_t panelRotation{};

    // Per-panel sensor settings:
    packed_sensor_settings_t panelSettings[9]{};

    // Internal tunable; should be left unchanged.
    uint8_t preDetailsDelayMilliseconds = 5;

    // Pad the struct to 250 bytes. This keeps the struct size stable as fields are added.
    // Applications should leave any data in here unchanged when calling SMX_SetConfig.
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

    /// New sensor test data has been received.
    /// Call SMX_GetTestData() to retrieve the data.
    SMXUpdateCallback_SensorTestData = 1 << 5,
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

/// Update panel LEDs on both pads. Both pads are always updated together.
///
/// lightData is a flat array of 8-bit RGB colors, one for each LED on each panel.
/// lightDataSize must be either:
///   - 1350 bytes: 2 pads × 9 panels × 25 LEDs × 3 RGB (firmware v4+ with inner 3×3 grid)
///   - 864 bytes:  2 pads × 9 panels × 16 LEDs × 3 RGB (legacy 4×4-only layout)
///
/// Each panel has LEDs in the following order (25-LED mode):
///
///   00  01  02  03      (outer 4×4 grid, rows 0-1)
///      16  17  18       (inner 3×3 grid, row 0)
///   04  05  06  07      (outer 4×4 grid, rows 2-3)
///      19  20  21       (inner 3×3 grid, row 1)
///   08  09  10  11      (outer 4×4 grid, rows 4-5)
///      22  23  24       (inner 3×3 grid, row 2)
///   12  13  14  15      (outer 4×4 grid, rows 6-7)
///
/// Panels are ordered left-to-right, top-to-bottom for each pad:
///   Pad 0: panels 0-8, Pad 1: panels 9-17
///
/// Lights update at up to 30 FPS. Faster calls replace pending data without increasing
/// the update rate. Panels return to automatic lighting if no updates are received for
/// a few seconds, so applications should send updates continuously.
///
/// A 0.6666 color scaling factor is applied to all values before sending (values above
/// ~170 don't make LEDs brighter; this improves contrast and reduces power draw).
///
/// @param lightData Pointer to the RGB light data buffer.
/// @param lightDataSize Size of the buffer in bytes (must be 1350 or 864).
SMX_API void SMX_SetLights2(const char *lightData, int lightDataSize);

/// (Deprecated) Equivalent to SMX_SetLights2(lightData, 864).
/// Use SMX_SetLights2 instead for 25-LED panel support.
///
/// @param lightData Pointer to 864 bytes of RGB data (2 pads × 9 panels × 16 LEDs × 3).
SMX_API void SMX_SetLights(const char lightData[864]);

/// Sets the platform edge LED strip colors for both pads.
/// The input buffer contains 88 RGB triplets (264 bytes total): the first 44 LEDs
/// (132 bytes) are for pad 0, the second 44 LEDs (132 bytes) are for pad 1.
/// Requires firmware v4+. Disables auto-lighting until timeout or SMX_ReenableAutoLights.
///
/// @param pLightData Pointer to 264 bytes of RGB data (88 LEDs × 3 bytes each).
SMX_API void SMX_SetPlatformLights(const char *pLightData);

/// Panel-side diagnostic test modes.
/// These activate debug lighting on the panels and don't return data to the host.
/// Lights cannot be updated while a panel test mode is active.
enum PanelTestMode {
    PanelTestMode_Off = '0',
    PanelTestMode_PressureTest = '1',
};

/// Sensor test modes for reading raw/calibrated sensor values.
/// The values (except Off) correspond with the protocol and must not be changed.
enum SensorTestMode {
    SensorTestMode_Off = 0,
    // Return the raw, uncalibrated value of each sensor.
    SensorTestMode_UncalibratedValues = '0',
    // Return the calibrated value of each sensor.
    SensorTestMode_CalibratedValues = '1',
    // Return the sensor noise value.
    SensorTestMode_Noise = '2',
    // Return the sensor tare value.
    SensorTestMode_Tare = '3',
};

/// Sensor test mode data returned by SMX_GetTestData.
/// Contains per-panel sensor readings and diagnostic information.
struct SMXSensorTestModeData
{
    // If false, we didn't receive a response from that panel.
    bool bHaveDataFromPanel[9];

    // Sensor readings for each panel. The meaning depends on the active SensorTestMode:
    //   UncalibratedValues: Raw ADC reading from the sensor (unscaled).
    //   CalibratedValues:   Value after calibration/tare subtraction. Represents force.
    //   Noise:              Standard deviation of recent readings, SQUARED.
    //                       Take sqrt() to get the actual noise level.
    //   Tare:               The current tare (baseline) value for each sensor.
    int16_t sensorLevel[9][4];

    // True if a sensor's most recent reading is invalid.
    bool bBadSensorInput[9][4];

    // The DIP switch settings on each panel (4-bit value).
    int iDIPSwitchPerPanel[9];

    // Bad sensor selection jumper indication for each sensor on each panel.
    bool iBadJumper[9][4];
};

/// Scales a raw sensor test value for display (e.g., in a bar graph).
/// Applies mode-specific transformations:
///   - Noise mode: returns sqrt(value) since raw values are variance (std dev squared).
///   - FSR panels (fw >= 4 with PlatformFlags_FSR): right-shifts by 2 and scales to 0.0-1.0 (max ~250).
///   - Load cell panels: scales to 0.0-1.0 (max ~500).
/// Small negative values (-10 to 0) are clamped to 0 (sensor noise artifact).
///
/// @param iValue Raw sensor value from SMXSensorTestModeData::sensorLevel.
/// @param mode The active sensor test mode.
/// @param bIsFSR True if the panel uses FSR sensors (check config flags & PlatformFlags_FSR, fw >= 4).
/// @return Normalized value in the range [0.0, 1.0+] suitable for display.
inline float SMX_ScaleSensorValue(int16_t iValue, SensorTestMode mode, bool bIsFSR)
{
    float fValue = static_cast<float>(iValue);

    if(mode == SensorTestMode_Noise)
        fValue = sqrtf(fValue > 0 ? fValue : 0);

    // Clamp small negative values from noise.
    if(fValue < 0 && fValue >= -10)
        fValue = 0;

    if(bIsFSR)
        return (fValue / 4.0f) / 250.0f;
    return fValue / 500.0f;
}

/// Sets a panel test mode on all connected pads.
/// When enabled, the SDK periodically resends the command to keep the mode active
/// (the device times out after ~1 second without a refresh).
/// Lights cannot be updated while a panel test mode is active.
///
/// @param mode The test mode to activate, or PanelTestMode_Off to disable.
SMX_API void SMX_SetPanelTestMode(PanelTestMode mode);

/// Sets the sensor test mode for a pad.
/// When enabled, the SDK periodically requests sensor data from the device.
/// Use SMX_GetTestData to retrieve the most recent readings.
/// Set to SensorTestMode_Off to stop requesting data.
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param mode The sensor test mode to activate.
SMX_API void SMX_SetTestMode(int pad, SensorTestMode mode);

/// Retrieves the most recent sensor test data for a pad.
/// Returns true if data is available (test mode is active and at least one
/// response has been received from the device).
///
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param data [out] Pointer to SMXSensorTestModeData to be filled.
/// @return True if data was retrieved, false if no data is available.
SMX_API bool SMX_GetTestData(int pad, SMXSensorTestModeData *data);

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
/// @return C-string containing the version (e.g., "1.0.0").
SMX_API const char *SMX_Version();

/// Returns the elapsed time in seconds since the SDK was initialized.
/// This is useful for logging timestamps and measuring elapsed time.
/// Uses a high-resolution monotonic clock that is not affected by system clock adjustments.
///
/// @return Elapsed time in seconds as a double (e.g., 1.234 for 1.234 seconds).
SMX_API double SMX_GetMonotonicTime();

// --- Panel Animation API ---

/// Animation types for panel lighting.
enum SMX_LightsType {
    SMX_LightsType_Released = 0, ///< Animation while panels are released (idle)
    SMX_LightsType_Pressed = 1,  ///< Animation while a panel is pressed
};

/// Load an animated GIF as a panel animation.
///
/// The GIF must be either 14×15 pixels (4×4 LED mode) or 23×24 pixels (25-LED mode),
/// containing a 3×3 grid of panels with 1-pixel gutters between them:
///
///   14×15 layout (4×4 LEDs per panel):
///     Each panel occupies a 4×4 pixel region at positions (col*5, row*5).
///     The 1-pixel gutters between panels are ignored.
///     Bottom row (y=14) is a flag row: if the bottom-left pixel is white,
///     that frame marks the loop point.
///
///   23×24 layout (25 LEDs per panel):
///     Each panel occupies an 8×8 pixel region at positions (col*8, row*8).
///     The outer 4×4 grid is sampled at even coordinates (dx*2, dy*2).
///     The inner 3×3 grid is sampled at odd coordinates (dx*2+1, dy*2+1).
///     Bottom row (y=23) is a flag row for loop point marking.
///
/// @param gif Pointer to raw GIF file data.
/// @param size Size of the GIF data in bytes.
/// @param pad Pad index (0 or 1).
/// @param type Which animation slot to load into (released or pressed).
/// @param error [out] On failure, set to a static error string. Valid until next call.
/// @return True on success, false on error (check *error for details).
SMX_API bool SMX_LightsAnimation_Load(const char *gif, int size, int pad, SMX_LightsType type, const char **error);

/// Enable or disable automatic panel animation playback.
///
/// When enabled, any animations loaded with SMX_LightsAnimation_Load will play
/// automatically at 30 FPS. The released animation plays continuously; the pressed
/// animation plays only while a panel is pressed and rewinds on release.
///
/// Animation playback is temporarily paused when SMX_SetLights2 is called directly,
/// resuming after ~100ms of no direct lights calls.
///
/// @param enable True to start automatic animation, false to stop.
SMX_API void SMX_LightsAnimation_SetAuto(bool enable);

/// Callback type for upload progress reporting.
/// @param progress Progress value from 0 to 100. 100 indicates completion.
/// @param pUser Application context pointer passed to SMX_LightsUpload_BeginUpload.
typedef void SMX_LightsUploadCallback(int progress, void *pUser);

/// Prepare an animation upload from a GIF file.
///
/// This converts the GIF into the firmware's internal format (4-bit paletted packed
/// sprites) and generates the upload command sequence. The GIF must be 23×24 pixels
/// (25-LED mode, the only format supported for firmware upload).
///
/// Both animation types (released and pressed) should be prepared before calling
/// BeginUpload. Each call to PrepareUpload appends to the upload command queue
/// for that pad, so you can prepare both types before a single BeginUpload call.
/// BeginUpload clears the queue after consuming it.
///
/// @param gif Pointer to raw GIF file data.
/// @param size Size of the GIF data in bytes.
/// @param pad Pad index (0 or 1).
/// @param type Which animation slot to prepare (released or pressed).
/// @param error [out] On failure, set to a static error string.
/// @return True on success, false on error.
SMX_API bool SMX_LightsUpload_PrepareUpload(const char *gif, int size, int pad, SMX_LightsType type, const char **error);

/// Begin uploading prepared animation data to the pad's firmware.
///
/// This queues all upload commands for the specified pad. The callback is invoked
/// as each command completes, with progress values from 0 to 100. The callback
/// will always be called exactly once with progress=100 when the upload finishes
/// (even if the pad disconnects mid-upload).
///
/// The callback is invoked from the I/O thread. It should return quickly.
///
/// Both animation types should be prepared via SMX_LightsUpload_PrepareUpload
/// before calling this function.
///
/// @param pad Pad index (0 or 1).
/// @param callback Progress callback function.
/// @param pUser Application context pointer passed to the callback.
SMX_API void SMX_LightsUpload_BeginUpload(int pad, SMX_LightsUploadCallback callback, void *pUser);

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
