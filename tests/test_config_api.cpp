#include <doctest/doctest.h>
#include "test_helpers_manager.h"
#include "SMXConfigPacket.h"

#include <cstring>
#include <vector>

using namespace std;
using namespace SMX;
using namespace SMXTestHelpers;

// --- Helpers ---

/// Builds config response packets containing a full SMXConfig struct.
/// Fragments into multiple HID packets like the real device would.
/// Returns a vector of packets to queue on the fake device.
static vector<vector<uint8_t>> MakeFullConfigResponsePackets(const SMXConfig &cfg)
{
    // Build the full payload: 'G' + size + config data
    vector<uint8_t> payload;
    payload.push_back('G');
    payload.push_back(static_cast<uint8_t>(sizeof(SMXConfig)));
    const auto *p = reinterpret_cast<const uint8_t*>(&cfg);
    payload.insert(payload.end(), p, p + sizeof(SMXConfig));

    // Fragment into HID packets (max 61 bytes payload per packet)
    vector<vector<uint8_t>> packets;
    for(size_t offset = 0; offset < payload.size(); offset += HID_MAX_PAYLOAD_SIZE)
    {
        size_t chunkSize = min(HID_MAX_PAYLOAD_SIZE, payload.size() - offset);
        uint8_t flags = 0;
        if(offset == 0)
            flags |= PACKET_FLAG_START_OF_COMMAND;
        if(offset + chunkSize == payload.size())
            flags |= PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED;

        vector<uint8_t> pkt(HID_PACKET_SIZE, 0);
        pkt[0] = HID_REPORT_DATA;
        pkt[1] = flags;
        pkt[2] = static_cast<uint8_t>(chunkSize);
        memcpy(&pkt[3], payload.data() + offset, chunkSize);
        packets.push_back(pkt);
    }
    return packets;
}

// =========================================================================
// SMX_GetConfig tests
// =========================================================================

TEST_CASE("SMX_GetConfig returns false when no device is connected") {
    auto pEnum = new FakeHIDEnumerator();
    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXConfig cfg = {};
    CHECK_FALSE(SMX_GetConfig(0, &cfg));
    CHECK_FALSE(SMX_GetConfig(1, &cfg));

    SMX_Stop();
}

TEST_CASE("SMX_GetConfig returns config after device connects") {
    SMXConfig deviceConfig = {};
    deviceConfig.panelDebounceMicroseconds = 7777;
    deviceConfig.autoCalibrationMaxDeviation = 42;
    deviceConfig.panelSettings[0].loadCellLowThreshold = 100;
    deviceConfig.panelSettings[4].loadCellHighThreshold = 200;

    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(deviceConfig));

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    SMXConfig readCfg = {};
    CHECK(SMX_GetConfig(0, &readCfg));
    CHECK(readCfg.panelDebounceMicroseconds == 7777);
    CHECK(readCfg.autoCalibrationMaxDeviation == 42);
    CHECK(readCfg.panelSettings[0].loadCellLowThreshold == 100);
    CHECK(readCfg.panelSettings[4].loadCellHighThreshold == 200);

    SMX_Stop();
}

TEST_CASE("SMX_GetConfig returns false for invalid pad index") {
    auto pEnum = new FakeHIDEnumerator();
    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXConfig cfg = {};
    CHECK_FALSE(SMX_GetConfig(2, &cfg));
    CHECK_FALSE(SMX_GetConfig(-1, &cfg));

    SMX_Stop();
}

// =========================================================================
// SMX_SetConfig tests
// =========================================================================

TEST_CASE("SMX_SetConfig sends write command to device") {
    SMXConfig deviceConfig = {};
    deviceConfig.panelDebounceMicroseconds = 4000;

    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(deviceConfig));
    pFakeDevice->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    // Clear writes from connection handshake
    pFakeDevice->ClearCapturedWrites();

    // Set a new config
    SMXConfig newCfg = deviceConfig;
    newCfg.panelDebounceMicroseconds = 9999;
    newCfg.panelSettings[3].loadCellLowThreshold = 55;
    SMX_SetConfig(0, &newCfg);

    // Wait for the write to be sent
    bool bWriteSent = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() > 0;
    });
    CHECK(bWriteSent);

    // Verify the write contains 'W' command
    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFoundWriteCmd = false;
    for(const auto &w : writes)
    {
        // HID packet: [report_id=5][flags][size][payload...]
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND && w[2] >= 1 && w[3] == 'W')
        {
            bFoundWriteCmd = true;
            break;
        }
    }
    CHECK(bFoundWriteCmd);

    SMX_Stop();
}

TEST_CASE("SMX_GetConfig returns pending config optimistically after SetConfig") {
    SMXConfig deviceConfig = {};
    deviceConfig.panelDebounceMicroseconds = 4000;

    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(deviceConfig));

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    // Set a new config
    SMXConfig newCfg = deviceConfig;
    newCfg.panelDebounceMicroseconds = 12345;
    SMX_SetConfig(0, &newCfg);

    // GetConfig should immediately return the new value (optimistic read)
    // Wait briefly for the write to be processed
    SMXConfig readCfg = {};
    bool bGotNewValue = WaitFor([&]() {
        SMX_GetConfig(0, &readCfg);
        return readCfg.panelDebounceMicroseconds == 12345;
    });
    CHECK(bGotNewValue);

    SMX_Stop();
}

TEST_CASE("SMX_SetConfig fires ConfigUpdated callback after write") {
    SMXConfig deviceConfig = {};

    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(deviceConfig));

    int iConfigUpdatedCount = 0;
    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_ConfigUpdated)
            (*static_cast<int*>(pUser))++;
    };

    SMX_StartWithEnumerator(callback, &iConfigUpdatedCount, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    // Reset counter after connection (which fires ConfigUpdated)
    int iCountAfterConnect = iConfigUpdatedCount;

    SMXConfig newCfg = {};
    newCfg.panelDebounceMicroseconds = 5555;
    SMX_SetConfig(0, &newCfg);

    // Wait for the config updated callback to fire again (from read-back)
    bool bGotCallback = WaitFor([&]() {
        return iConfigUpdatedCount > iCountAfterConnect;
    });
    CHECK(bGotCallback);

    SMX_Stop();
}

// =========================================================================
// ConvertToOldConfig tests
// =========================================================================

TEST_CASE("ConvertToOldConfig maps panel thresholds correctly") {
    SMXConfig newCfg = {};
    newCfg.debounceNodelayMilliseconds = 42;
    newCfg.panelDebounceMicroseconds = 5000;
    newCfg.autoCalibrationMaxDeviation = 99;
    newCfg.badSensorMinimumDelaySeconds = 12;
    newCfg.autoCalibrationAveragesPerUpdate = 55;
    newCfg.autoCalibrationSamplesPerAverage = 400;
    newCfg.panelRotation = 2;
    newCfg.autoLightsTimeout = 8;
    newCfg.debounceDelayMilliseconds = 250;
    newCfg.masterVersion = 4;
    newCfg.configVersion = 3;

    for(int i = 0; i < 9; i++)
    {
        newCfg.panelSettings[i].loadCellLowThreshold = static_cast<uint8_t>(10 + i);
        newCfg.panelSettings[i].loadCellHighThreshold = static_cast<uint8_t>(110 + i);
    }

    vector<uint8_t> oldData(sizeof(OldSMXConfig), 0);
    ConvertToOldConfig(newCfg, oldData);

    const OldSMXConfig &old = *reinterpret_cast<const OldSMXConfig*>(oldData.data());

    CHECK(old.masterDebounceMilliseconds == 42);
    CHECK(old.panelDebounceMicroseconds == 5000);
    CHECK(old.autoCalibrationMaxDeviation == 99);
    CHECK(old.badSensorMinimumDelaySeconds == 12);
    CHECK(old.autoCalibrationAveragesPerUpdate == 55);
    CHECK(old.autoCalibrationSamplesPerAverage == 400);
    CHECK(old.panelRotation == 2);
    CHECK(old.autoLightsTimeout == 8);
    CHECK(old.debounceDelayMilliseconds == 250);
    CHECK(old.masterVersion == 4);
    CHECK(old.configVersion == 3);

    CHECK(old.panelThreshold0Low == 10);
    CHECK(old.panelThreshold1Low == 11);
    CHECK(old.panelThreshold2Low == 12);
    CHECK(old.panelThreshold3Low == 13);
    CHECK(old.panelThreshold4Low == 14);
    CHECK(old.panelThreshold5Low == 15);
    CHECK(old.panelThreshold6Low == 16);
    CHECK(old.panelThreshold7Low == 17);
    CHECK(old.panelThreshold8Low == 18);

    CHECK(old.panelThreshold0High == 110);
    CHECK(old.panelThreshold1High == 111);
    CHECK(old.panelThreshold2High == 112);
    CHECK(old.panelThreshold3High == 113);
    CHECK(old.panelThreshold4High == 114);
    CHECK(old.panelThreshold5High == 115);
    CHECK(old.panelThreshold6High == 116);
    CHECK(old.panelThreshold7High == 117);
    CHECK(old.panelThreshold8High == 118);
}

TEST_CASE("ConvertToOldConfig extends small buffer to 128 bytes") {
    SMXConfig newCfg = {};
    vector<uint8_t> oldData(10, 0);
    ConvertToOldConfig(newCfg, oldData);
    CHECK(oldData.size() >= 128);
}

TEST_CASE("ConvertToOldConfig round-trips with ConvertToNewConfig") {
    SMXConfig original = {};
    original.debounceNodelayMilliseconds = 100;
    original.panelDebounceMicroseconds = 6000;
    original.autoCalibrationMaxDeviation = 80;
    original.badSensorMinimumDelaySeconds = 20;
    original.autoCalibrationAveragesPerUpdate = 70;
    original.autoCalibrationSamplesPerAverage = 300;
    original.panelRotation = 1;
    original.autoLightsTimeout = 5;
    original.debounceDelayMilliseconds = 150;
    original.masterVersion = 5;
    original.configVersion = 3;

    for(int i = 0; i < 9; i++)
    {
        original.panelSettings[i].loadCellLowThreshold = static_cast<uint8_t>(20 + i * 3);
        original.panelSettings[i].loadCellHighThreshold = static_cast<uint8_t>(120 + i * 3);
    }
    original.enabledSensors[0] = 0xAB;
    original.enabledSensors[4] = 0xCD;
    original.stepColor[0] = 255;
    original.stepColor[1] = 128;

    // Convert to old format
    vector<uint8_t> oldData(sizeof(OldSMXConfig), 0);
    ConvertToOldConfig(original, oldData);

    // Convert back to new format
    SMXConfig roundTripped = {};
    ConvertToNewConfig(oldData, roundTripped);

    // Fields that survive the round-trip
    CHECK(roundTripped.debounceNodelayMilliseconds == original.debounceNodelayMilliseconds);
    CHECK(roundTripped.panelDebounceMicroseconds == original.panelDebounceMicroseconds);
    CHECK(roundTripped.autoCalibrationMaxDeviation == original.autoCalibrationMaxDeviation);
    CHECK(roundTripped.badSensorMinimumDelaySeconds == original.badSensorMinimumDelaySeconds);
    CHECK(roundTripped.autoCalibrationAveragesPerUpdate == original.autoCalibrationAveragesPerUpdate);
    CHECK(roundTripped.autoCalibrationSamplesPerAverage == original.autoCalibrationSamplesPerAverage);
    CHECK(roundTripped.panelRotation == original.panelRotation);
    CHECK(roundTripped.autoLightsTimeout == original.autoLightsTimeout);
    CHECK(roundTripped.debounceDelayMilliseconds == original.debounceDelayMilliseconds);
    CHECK(roundTripped.enabledSensors[0] == original.enabledSensors[0]);
    CHECK(roundTripped.enabledSensors[4] == original.enabledSensors[4]);
    CHECK(roundTripped.stepColor[0] == original.stepColor[0]);
    CHECK(roundTripped.stepColor[1] == original.stepColor[1]);

    for(int i = 0; i < 9; i++)
    {
        CHECK(roundTripped.panelSettings[i].loadCellLowThreshold == original.panelSettings[i].loadCellLowThreshold);
        CHECK(roundTripped.panelSettings[i].loadCellHighThreshold == original.panelSettings[i].loadCellHighThreshold);
    }
}
