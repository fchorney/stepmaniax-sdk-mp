#ifndef SMXConfigPacket_h
#define SMXConfigPacket_h

#include "SMX.h"
#include <vector>

// SMXConfig and packed_sensor_settings_t are defined in the public header (SMX.h).
// This file provides the old config format and conversion utilities.

/// Old configuration format used in firmware versions before v5.
/// This struct defines how config data was laid out in earlier firmware versions.
/// Used primarily for firmware upgrade handling in ConvertToNewConfig.
/// Old firmware config format (v3 and earlier).
/// This struct maps to the raw bytes sent/received by firmware versions < 5.
/// Fields are in wire order and must not be reordered.
/// See ConvertToNewConfig/ConvertToOldConfig for the mapping to SMXConfig.
#pragma pack(push, 1)
struct OldSMXConfig
{
    // --- Unused legacy fields (preserved for wire compatibility) ---
    uint8_t unused1 = 0xFF, unused2 = 0xFF;
    uint8_t unused3 = 0xFF, unused4 = 0xFF;
    uint8_t unused5 = 0xFF, unused6 = 0xFF;

    // --- Debounce ---
    uint16_t masterDebounceMilliseconds = 0;  // → SMXConfig::debounceNodelayMilliseconds

    // --- Panel thresholds (original 3 panels: up/left/right) ---
    uint8_t panelThreshold7Low = 0xFF, panelThreshold7High = 0xFF;   // Panel 7 (up)
    uint8_t panelThreshold4Low = 0xFF, panelThreshold4High = 0xFF;   // Panel 4 (left)
    uint8_t panelThreshold2Low = 0xFF, panelThreshold2High = 0xFF;   // Panel 2 (right)

    // --- Calibration and timing ---
    uint16_t panelDebounceMicroseconds = 4000;
    uint16_t autoCalibrationPeriodMilliseconds = 1000;  // Not mapped to new config
    uint8_t autoCalibrationMaxDeviation = 100;
    uint8_t badSensorMinimumDelaySeconds = 15;
    uint16_t autoCalibrationAveragesPerUpdate = 60;

    // --- More unused legacy fields ---
    uint8_t unused7 = 0xFF, unused8 = 0xFF;

    // --- Panel 1 (down) threshold ---
    uint8_t panelThreshold1Low = 0xFF, panelThreshold1High = 0xFF;

    // --- Sensor and display settings ---
    uint8_t enabledSensors[5]{};
    uint8_t autoLightsTimeout = 1000/128;
    uint8_t stepColor[3*9]{};
    uint8_t panelRotation{};
    uint16_t autoCalibrationSamplesPerAverage = 500;

    // --- Version info ---
    uint8_t masterVersion = 0xFF;
    uint8_t configVersion = 0x03;  // Determines which fields below are valid

    // --- Unused padding ---
    uint8_t unused9[10]{};

    // --- Per-panel thresholds (configVersion >= 2) ---
    uint8_t panelThreshold0Low{}, panelThreshold0High{};   // Panel 0 (up-left)
    uint8_t panelThreshold3Low{}, panelThreshold3High{};   // Panel 3 (center-left)
    uint8_t panelThreshold5Low{}, panelThreshold5High{};   // Panel 5 (center-right)
    uint8_t panelThreshold6Low{}, panelThreshold6High{};   // Panel 6 (down-left)
    uint8_t panelThreshold8Low{}, panelThreshold8High{};   // Panel 8 (down-right)

    // --- Debounce delay (configVersion >= 3) ---
    uint16_t debounceDelayMilliseconds = 0;

    // --- Padding to fixed size ---
    uint8_t padding[164]{};
};
#pragma pack(pop)

/// Converts old config format (v3) to new config format (v5).
/// This function handles firmware version upgrades by mapping old config fields to new ones.
/// If the old config uses an uninitialized version (0xFF), basic defaults are applied.
/// Supports incremental upgrades through intermediate versions.
///
/// Version history:
/// - v3 and earlier: Original sensor threshold layout
/// - v2: Additional panel thresholds added (panels 0, 3, 5, 6, 8)
/// - v3: debounceDelayMilliseconds field added
/// - v5 (current): Sensor settings restructured into packed_sensor_settings_t per panel
///
/// @param oldConfig Raw old config data (as vector of bytes)
/// @param newConfig Output config struct to be populated with converted values
void ConvertToNewConfig(const std::vector<uint8_t> &oldConfig, SMXConfig &newConfig);

/// Converts new config format (v5) to old config format (v3) for writing to old firmware.
/// Maps new panelSettings thresholds back to the old per-panel threshold fields.
///
/// @param newConfig The new-format config to convert from.
/// @param oldConfigData [in/out] Raw old config data to be updated. Extended to 128 bytes if smaller.
void ConvertToOldConfig(const SMXConfig &newConfig, std::vector<uint8_t> &oldConfigData);

#endif
