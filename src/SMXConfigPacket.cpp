#include "SMXConfigPacket.h"
#include <cstring>

using namespace std;

/// Converts old config format (v3 and earlier) to new config format (v5).
/// This handles firmware upgrades by mapping old field locations to new locations.
/// The conversion is incremental, supporting intermediate versions.
///
/// Algorithm:
/// 1. Always copy common fields (debounce, colors, calibration settings, etc.)
/// 2. If configVersion is uninitialized (0xFF), skip version-specific fields
/// 3. If configVersion < 2, skip newer panel threshold fields
/// 4. If configVersion < 3, skip debounceDelayMilliseconds
///
/// @param oldConfig Raw old config data (vector of bytes from device)
/// @param newConfig Output config struct to be populated
void ConvertToNewConfig(const std::vector<uint8_t> &oldConfig, SMXConfig &newConfig)
{
    const OldSMXConfig &old = *reinterpret_cast<const OldSMXConfig*>(oldConfig.data());

    // Copy base fields present in all versions.
    newConfig.debounceNodelayMilliseconds = old.masterDebounceMilliseconds;

    // Panel 7 (up), 4 (left), 2 (right) thresholds (present in original version).
    newConfig.panelSettings[7].loadCellLowThreshold = old.panelThreshold7Low;
    newConfig.panelSettings[4].loadCellLowThreshold = old.panelThreshold4Low;
    newConfig.panelSettings[2].loadCellLowThreshold = old.panelThreshold2Low;
    newConfig.panelSettings[7].loadCellHighThreshold = old.panelThreshold7High;
    newConfig.panelSettings[4].loadCellHighThreshold = old.panelThreshold4High;
    newConfig.panelSettings[2].loadCellHighThreshold = old.panelThreshold2High;

    // Copy calibration and debounce settings.
    newConfig.panelDebounceMicroseconds = old.panelDebounceMicroseconds;
    newConfig.autoCalibrationMaxDeviation = old.autoCalibrationMaxDeviation;
    newConfig.badSensorMinimumDelaySeconds = old.badSensorMinimumDelaySeconds;
    newConfig.autoCalibrationAveragesPerUpdate = old.autoCalibrationAveragesPerUpdate;

    // Panel 1 (down) thresholds.
    newConfig.panelSettings[1].loadCellLowThreshold = old.panelThreshold1Low;
    newConfig.panelSettings[1].loadCellHighThreshold = old.panelThreshold1High;

    // Copy sensor settings and display configuration.
    memcpy(newConfig.enabledSensors, old.enabledSensors, sizeof(newConfig.enabledSensors));
    newConfig.autoLightsTimeout = old.autoLightsTimeout;
    memcpy(newConfig.stepColor, old.stepColor, sizeof(newConfig.stepColor));
    newConfig.panelRotation = old.panelRotation;
    newConfig.autoCalibrationSamplesPerAverage = old.autoCalibrationSamplesPerAverage;

    // If version is uninitialized, stop here.
    if(old.configVersion == 0xFF)
        return;

    // Copy version information.
    newConfig.masterVersion = old.masterVersion;
    newConfig.configVersion = old.configVersion;

    // Version 1: no additional fields beyond the above.
    if(old.configVersion < 2)
        return;

    // Version 2 and later: all 9 panel thresholds present (previously only panels 1,2,4,7 were stored).
    newConfig.panelSettings[0].loadCellLowThreshold = old.panelThreshold0Low;
    newConfig.panelSettings[3].loadCellLowThreshold = old.panelThreshold3Low;
    newConfig.panelSettings[5].loadCellLowThreshold = old.panelThreshold5Low;
    newConfig.panelSettings[6].loadCellLowThreshold = old.panelThreshold6Low;
    newConfig.panelSettings[8].loadCellLowThreshold = old.panelThreshold8Low;
    newConfig.panelSettings[0].loadCellHighThreshold = old.panelThreshold0High;
    newConfig.panelSettings[3].loadCellHighThreshold = old.panelThreshold3High;
    newConfig.panelSettings[5].loadCellHighThreshold = old.panelThreshold5High;
    newConfig.panelSettings[6].loadCellHighThreshold = old.panelThreshold6High;
    newConfig.panelSettings[8].loadCellHighThreshold = old.panelThreshold8High;

    // Version 3 and later: debounce delay field added.
    if(old.configVersion < 3)
        return;

    newConfig.debounceDelayMilliseconds = old.debounceDelayMilliseconds;
}
