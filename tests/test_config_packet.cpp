#include <doctest/doctest.h>
#include "SMXConfigPacket.h"

#include <cstring>
#include <vector>

using namespace std;

// OldSMXConfig layout (mirrors the struct in SMXConfigPacket.cpp)
// We build raw byte vectors to feed into ConvertToNewConfig.
#pragma pack(push, 1)
struct OldSMXConfig
{
    uint8_t unused1, unused2, unused3, unused4, unused5, unused6;
    uint16_t masterDebounceMilliseconds;
    uint8_t panelThreshold7Low, panelThreshold7High;
    uint8_t panelThreshold4Low, panelThreshold4High;
    uint8_t panelThreshold2Low, panelThreshold2High;
    uint16_t panelDebounceMicroseconds;
    uint16_t autoCalibrationPeriodMilliseconds;
    uint8_t autoCalibrationMaxDeviation;
    uint8_t badSensorMinimumDelaySeconds;
    uint16_t autoCalibrationAveragesPerUpdate;
    uint8_t unused7, unused8;
    uint8_t panelThreshold1Low, panelThreshold1High;
    uint8_t enabledSensors[5];
    uint8_t autoLightsTimeout;
    uint8_t stepColor[3*9];
    uint8_t panelRotation;
    uint16_t autoCalibrationSamplesPerAverage;
    uint8_t masterVersion;
    uint8_t configVersion;
    uint8_t unused9[10];
    uint8_t panelThreshold0Low, panelThreshold0High;
    uint8_t panelThreshold3Low, panelThreshold3High;
    uint8_t panelThreshold5Low, panelThreshold5High;
    uint8_t panelThreshold6Low, panelThreshold6High;
    uint8_t panelThreshold8Low, panelThreshold8High;
    uint16_t debounceDelayMilliseconds;
    uint8_t padding[164];
};
#pragma pack(pop)

static vector<uint8_t> MakeOldConfig(const OldSMXConfig &cfg)
{
    vector<uint8_t> raw(sizeof(OldSMXConfig));
    memcpy(raw.data(), &cfg, sizeof(cfg));
    return raw;
}

TEST_CASE("ConvertToNewConfig: uninitialized version (0xFF) copies base fields only") {
    OldSMXConfig old = {};
    old.configVersion = 0xFF;
    old.masterDebounceMilliseconds = 42;
    old.panelThreshold7Low = 10;
    old.panelThreshold7High = 20;
    old.panelThreshold4Low = 30;
    old.panelThreshold4High = 40;
    old.panelThreshold2Low = 50;
    old.panelThreshold2High = 60;
    old.panelThreshold1Low = 70;
    old.panelThreshold1High = 80;
    old.panelDebounceMicroseconds = 5000;
    old.autoCalibrationMaxDeviation = 99;
    old.badSensorMinimumDelaySeconds = 12;
    old.autoCalibrationAveragesPerUpdate = 55;
    old.autoCalibrationSamplesPerAverage = 400;
    old.panelRotation = 2;
    old.autoLightsTimeout = 8;

    SMXConfig newCfg = {};
    ConvertToNewConfig(MakeOldConfig(old), newCfg);

    CHECK(newCfg.debounceNodelayMilliseconds == 42);
    CHECK(newCfg.panelSettings[7].loadCellLowThreshold == 10);
    CHECK(newCfg.panelSettings[7].loadCellHighThreshold == 20);
    CHECK(newCfg.panelSettings[4].loadCellLowThreshold == 30);
    CHECK(newCfg.panelSettings[4].loadCellHighThreshold == 40);
    CHECK(newCfg.panelSettings[2].loadCellLowThreshold == 50);
    CHECK(newCfg.panelSettings[2].loadCellHighThreshold == 60);
    CHECK(newCfg.panelSettings[1].loadCellLowThreshold == 70);
    CHECK(newCfg.panelSettings[1].loadCellHighThreshold == 80);
    CHECK(newCfg.panelDebounceMicroseconds == 5000);
    CHECK(newCfg.autoCalibrationMaxDeviation == 99);
    CHECK(newCfg.badSensorMinimumDelaySeconds == 12);
    CHECK(newCfg.autoCalibrationAveragesPerUpdate == 55);
    CHECK(newCfg.autoCalibrationSamplesPerAverage == 400);
    CHECK(newCfg.panelRotation == 2);
    CHECK(newCfg.autoLightsTimeout == 8);

    // masterVersion and configVersion should NOT be copied for 0xFF
    CHECK(newCfg.masterVersion == 0xFF);  // default
    CHECK(newCfg.configVersion == 0x05);  // default

    // v2 panel thresholds should NOT be set
    CHECK(newCfg.panelSettings[0].loadCellLowThreshold == 0);
    CHECK(newCfg.panelSettings[3].loadCellLowThreshold == 0);
}

TEST_CASE("ConvertToNewConfig: version 1 copies version info but not v2 panels") {
    OldSMXConfig old = {};
    old.configVersion = 1;
    old.masterVersion = 3;
    old.masterDebounceMilliseconds = 100;
    old.panelThreshold7Low = 15;
    old.panelThreshold0Low = 99;  // should NOT be copied for v1

    SMXConfig newCfg = {};
    ConvertToNewConfig(MakeOldConfig(old), newCfg);

    CHECK(newCfg.masterVersion == 3);
    CHECK(newCfg.configVersion == 1);
    CHECK(newCfg.debounceNodelayMilliseconds == 100);
    CHECK(newCfg.panelSettings[7].loadCellLowThreshold == 15);
    // v2 fields not copied
    CHECK(newCfg.panelSettings[0].loadCellLowThreshold == 0);
    // v3 field not copied
    CHECK(newCfg.debounceDelayMilliseconds == 0);
}

TEST_CASE("ConvertToNewConfig: version 2 copies all panel thresholds but not debounceDelay") {
    OldSMXConfig old = {};
    old.configVersion = 2;
    old.masterVersion = 4;
    old.panelThreshold0Low = 11;
    old.panelThreshold0High = 12;
    old.panelThreshold3Low = 13;
    old.panelThreshold3High = 14;
    old.panelThreshold5Low = 15;
    old.panelThreshold5High = 16;
    old.panelThreshold6Low = 17;
    old.panelThreshold6High = 18;
    old.panelThreshold8Low = 19;
    old.panelThreshold8High = 20;
    old.debounceDelayMilliseconds = 999;  // should NOT be copied for v2

    SMXConfig newCfg = {};
    ConvertToNewConfig(MakeOldConfig(old), newCfg);

    CHECK(newCfg.panelSettings[0].loadCellLowThreshold == 11);
    CHECK(newCfg.panelSettings[0].loadCellHighThreshold == 12);
    CHECK(newCfg.panelSettings[3].loadCellLowThreshold == 13);
    CHECK(newCfg.panelSettings[3].loadCellHighThreshold == 14);
    CHECK(newCfg.panelSettings[5].loadCellLowThreshold == 15);
    CHECK(newCfg.panelSettings[5].loadCellHighThreshold == 16);
    CHECK(newCfg.panelSettings[6].loadCellLowThreshold == 17);
    CHECK(newCfg.panelSettings[6].loadCellHighThreshold == 18);
    CHECK(newCfg.panelSettings[8].loadCellLowThreshold == 19);
    CHECK(newCfg.panelSettings[8].loadCellHighThreshold == 20);
    CHECK(newCfg.debounceDelayMilliseconds == 0);
}

TEST_CASE("ConvertToNewConfig: version 3 copies debounceDelayMilliseconds") {
    OldSMXConfig old = {};
    old.configVersion = 3;
    old.masterVersion = 5;
    old.debounceDelayMilliseconds = 250;
    old.panelThreshold0Low = 22;

    SMXConfig newCfg = {};
    ConvertToNewConfig(MakeOldConfig(old), newCfg);

    CHECK(newCfg.debounceDelayMilliseconds == 250);
    CHECK(newCfg.panelSettings[0].loadCellLowThreshold == 22);
    CHECK(newCfg.masterVersion == 5);
    CHECK(newCfg.configVersion == 3);
}

TEST_CASE("ConvertToNewConfig: enabledSensors and stepColor are copied") {
    OldSMXConfig old = {};
    old.configVersion = 0xFF;
    old.enabledSensors[0] = 0xAA;
    old.enabledSensors[4] = 0xBB;
    old.stepColor[0] = 255;
    old.stepColor[1] = 128;
    old.stepColor[2] = 64;

    SMXConfig newCfg = {};
    ConvertToNewConfig(MakeOldConfig(old), newCfg);

    CHECK(newCfg.enabledSensors[0] == 0xAA);
    CHECK(newCfg.enabledSensors[4] == 0xBB);
    CHECK(newCfg.stepColor[0] == 255);
    CHECK(newCfg.stepColor[1] == 128);
    CHECK(newCfg.stepColor[2] == 64);
}
