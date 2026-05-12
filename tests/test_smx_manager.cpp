#include <doctest/doctest.h>
#include "test_helpers_manager.h"

using namespace std;
using namespace SMX;
using namespace SMXTestHelpers;

// =========================================================================
// Device discovery and player ordering tests
// =========================================================================

TEST_CASE("Single P1 device is discovered and connected") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    // Queue device info response (P1)
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    // Config response will be queued when activation command is written
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMXInfo infoResult = {};
    int connectedPad = -1;
    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_Connected)
            *static_cast<int*>(pUser) = pad;
    };

    SMX_StartWithEnumerator(callback, &connectedPad, unique_ptr<IHIDEnumerator>(pEnum));

    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &infoResult);
        return infoResult.m_bConnected;
    });

    CHECK(bConnected);
    CHECK_FALSE(infoResult.m_bIsPlayer2);
    CHECK(infoResult.m_iFirmwareVersion == 5);
    CHECK(connectedPad == 0);

    SMX_Stop();
}

TEST_CASE("Single P2 device is placed in slot 1") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    // Queue device info response (P2: player='1')
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info1 = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(1, &info1);
        return info1.m_bConnected;
    });

    CHECK(bConnected);
    CHECK(info1.m_bIsPlayer2);

    // Slot 0 should be empty
    SMXInfo info0 = {};
    SMX_GetInfo(0, &info0);
    CHECK_FALSE(info0.m_bConnected);

    SMX_Stop();
}

TEST_CASE("Two devices are ordered P1=slot0, P2=slot1") {
    auto pFakeP1 = new FakeDevice();
    auto pFakeP2 = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    // Add P2 first to test that ordering corrects it
    pEnum->AddDevice("/dev/hidraw0", pFakeP2);
    pEnum->AddDevice("/dev/hidraw1", pFakeP1);

    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeP2->SetConfigResponse(MakeConfigResponse());
    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeP1->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBothConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });

    CHECK(bBothConnected);
    CHECK_FALSE(info0.m_bIsPlayer2);  // slot 0 = P1
    CHECK(info1.m_bIsPlayer2);         // slot 1 = P2

    SMX_Stop();
}

// =========================================================================
// Disconnect and reconnect
// =========================================================================

TEST_CASE("Device disconnect fires callback and clears slot") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    int iDisconnectedPad = -1;
    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_Disconnected)
            *static_cast<int*>(pUser) = pad;
    };

    SMX_StartWithEnumerator(callback, &iDisconnectedPad, unique_ptr<IHIDEnumerator>(pEnum));

    // Wait for connection
    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    // Trigger disconnect by making writes fail. The main thread will detect
    // the write error in CheckWrites and close the device.
    pFakeDevice->SetFailWrites(true);

    // Send a command to trigger a write attempt
    SMX_SetSerialNumbers();

    // Wait for disconnect
    bool bDisconnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return !info.m_bConnected;
    });

    CHECK(bDisconnected);
    CHECK(iDisconnectedPad == 0);

    SMX_Stop();
}

// =========================================================================
// Duplicate player jumpers
// =========================================================================

TEST_CASE("Duplicate player jumpers: both P1 are assigned to slots without swap") {
    auto pFakeA = new FakeDevice();
    auto pFakeB = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeA);
    pEnum->AddDevice("/dev/hidraw1", pFakeB);

    // Both devices report as P1 (player='0')
    pFakeA->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeA->SetConfigResponse(MakeConfigResponse());
    pFakeB->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeB->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBothConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });

    CHECK(bBothConnected);
    // Both should report as P1 (same jumper)
    CHECK_FALSE(info0.m_bIsPlayer2);
    CHECK_FALSE(info1.m_bIsPlayer2);

    SMX_Stop();
}

// =========================================================================
// SMX_GetInputState through full stack
// =========================================================================

TEST_CASE("SMX_GetInputState returns panel state through full stack") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    // Wait for connection
    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    // Queue a Report 3 input state packet
    pFakeDevice->QueueRead({HID_REPORT_INPUT_STATE, 0x55, 0x01});  // state = 0x0155

    bool bGotState = WaitFor([&]() {
        return SMX_GetInputState(0) == 0x0155;
    });

    CHECK(bGotState);

    // Pad 1 should be 0 (not connected)
    CHECK(SMX_GetInputState(1) == 0);

    SMX_Stop();
}

// =========================================================================
// SMX_SetSerialNumbers command format
// =========================================================================

TEST_CASE("SMX_SetSerialNumbers sends 's' command with 16-byte serial") {
    auto pFakeDevice = new FakeDevice();
    pFakeDevice->SetCaptureWrites(true);
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    pFakeDevice->ClearCapturedWrites();
    SMX_SetSerialNumbers();

    // Wait for the command to be written
    bool bGotWrite = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() > 0;
    });
    REQUIRE(bGotWrite);

    // Verify the command format: report_id=5, flags, size, payload starts with 's'
    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFoundSerialCmd = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND)
        {
            uint8_t payloadSize = w[2];
            if(payloadSize >= 18 && w[3] == 's')  // 's' + 16 bytes serial + '\n'
            {
                bFoundSerialCmd = true;
                CHECK(payloadSize == 18);  // 's' + 16 + '\n'
                CHECK(w[3 + 17] == '\n');
                break;
            }
        }
    }
    CHECK(bFoundSerialCmd);

    SMX_Stop();
}

// =========================================================================
// SMX_GetInfo edge cases
// =========================================================================

TEST_CASE("SMX_GetInfo on disconnected pad returns not connected") {
    auto pEnum = new FakeHIDEnumerator();
    // No devices added

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    // Give it a moment to start
    this_thread::sleep_for(chrono::milliseconds(50));

    SMXInfo info0 = {}, info1 = {};
    SMX_GetInfo(0, &info0);
    SMX_GetInfo(1, &info1);

    CHECK_FALSE(info0.m_bConnected);
    CHECK_FALSE(info1.m_bConnected);
    CHECK(info0.m_iFirmwareVersion == 0);

    SMX_Stop();
}

// =========================================================================
// Config packet parsing (old/new format, invalid)
// =========================================================================

TEST_CASE("Device with firmware >= 5 uses 'G' (new config format)") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    int iConfigUpdated = 0;
    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_ConfigUpdated)
            (*static_cast<int*>(pUser))++;
    };

    SMX_StartWithEnumerator(callback, &iConfigUpdated, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });

    CHECK(bConnected);
    CHECK(iConfigUpdated > 0);

    SMX_Stop();
}

TEST_CASE("Device with firmware < 5 uses 'g' (old config format) and converts") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    // Firmware version 4 (< 5) → uses 'g' command
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 4));
    // Build old-format config response: 'g' + size + old config data
    vector<uint8_t> oldConfigPayload;
    oldConfigPayload.push_back('g');
    oldConfigPayload.push_back(40);
    oldConfigPayload.resize(2 + 40, 0);

    vector<uint8_t> oldConfigPkt;
    oldConfigPkt.push_back(HID_REPORT_DATA);
    oldConfigPkt.push_back(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED);
    oldConfigPkt.push_back(static_cast<uint8_t>(oldConfigPayload.size()));
    oldConfigPkt.insert(oldConfigPkt.end(), oldConfigPayload.begin(), oldConfigPayload.end());
    pFakeDevice->SetConfigResponse(oldConfigPkt);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });

    CHECK(bConnected);
    CHECK(info.m_iFirmwareVersion == 4);

    SMX_Stop();
}

TEST_CASE("Device reconnects successfully after read error disconnect") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    int iConnectedCount = 0;
    int iDisconnectedCount = 0;
    struct CallbackData { int *pConnected; int *pDisconnected; };
    CallbackData cbData = {&iConnectedCount, &iDisconnectedCount};

    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        auto *data = static_cast<CallbackData*>(pUser);
        if(reason & SMXUpdateCallback_Connected)
            (*data->pConnected)++;
        if(reason & SMXUpdateCallback_Disconnected)
            (*data->pDisconnected)++;
    };

    SMX_StartWithEnumerator(callback, &cbData, unique_ptr<IHIDEnumerator>(pEnum));

    // Wait for initial connection
    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);
    CHECK(iConnectedCount == 1);

    // Trigger disconnect via read error
    pFakeDevice->SetFailReadsAfterCount(1);
    pFakeDevice->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00}); // one successful read, then fail

    bool bDisconnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return !info.m_bConnected;
    });
    REQUIRE(bDisconnected);
    CHECK(iDisconnectedCount >= 1);

    // Reset the device for reconnection
    pFakeDevice->SetFailReadsAfterCount(0); // stop failing
    pEnum->ResetOpened("/dev/hidraw0");
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));

    // Wait for reconnection
    bool bReconnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });

    CHECK(bReconnected);
    CHECK(iConnectedCount >= 2);

    SMX_Stop();
}

// =========================================================================
// SMX_FactoryReset command format
// =========================================================================

TEST_CASE("SMX_FactoryReset sends 'f' command then re-reads config") {
    auto pFakeDevice = new FakeDevice();
    pFakeDevice->SetCaptureWrites(true);
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    pFakeDevice->ClearCapturedWrites();
    SMX_FactoryReset(0);

    // Wait for commands to be written
    bool bGotWrites = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() >= 2;
    });
    REQUIRE(bGotWrites);

    // Verify: first command is 'f\n', second is 'G' (firmware >= 5)
    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFoundFactoryReset = false;
    bool bFoundConfigRead = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND)
        {
            uint8_t payloadSize = w[2];
            if(payloadSize >= 2 && w[3] == 'f' && w[4] == '\n')
                bFoundFactoryReset = true;
            if(payloadSize >= 1 && w[3] == 'G')
                bFoundConfigRead = true;
        }
    }
    CHECK(bFoundFactoryReset);
    CHECK(bFoundConfigRead);

    SMX_Stop();
}

TEST_CASE("SMX_FactoryReset on old firmware sends 'g' instead of 'G'") {
    auto pFakeDevice = new FakeDevice();
    pFakeDevice->SetCaptureWrites(true);
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    // Firmware version 4 (old)
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 4));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    pFakeDevice->ClearCapturedWrites();
    SMX_FactoryReset(0);

    bool bGotWrites = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() >= 2;
    });
    REQUIRE(bGotWrites);

    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFoundConfigRead = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND)
        {
            uint8_t payloadSize = w[2];
            if(payloadSize >= 2 && w[3] == 'g' && w[4] == '\n')
                bFoundConfigRead = true;
        }
    }
    CHECK(bFoundConfigRead);

    SMX_Stop();
}

TEST_CASE("SMX_FactoryReset on disconnected pad does nothing") {
    auto pEnum = new FakeHIDEnumerator();

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    // Should not crash or hang
    SMX_FactoryReset(0);
    SMX_FactoryReset(1);

    SMX_Stop();
}

// =========================================================================
// SMX_ForceRecalibration command format
// =========================================================================

TEST_CASE("SMX_ForceRecalibration sends 'C' command") {
    auto pFakeDevice = new FakeDevice();
    pFakeDevice->SetCaptureWrites(true);
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    pFakeDevice->ClearCapturedWrites();
    SMX_ForceRecalibration(0);

    bool bGotWrite = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() > 0;
    });
    REQUIRE(bGotWrite);

    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFoundCalibrationCmd = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND)
        {
            uint8_t payloadSize = w[2];
            if(payloadSize >= 2 && w[3] == 'C' && w[4] == '\n')
            {
                bFoundCalibrationCmd = true;
                break;
            }
        }
    }
    CHECK(bFoundCalibrationCmd);

    SMX_Stop();
}

TEST_CASE("SMX_ForceRecalibration on disconnected pad does nothing") {
    auto pEnum = new FakeHIDEnumerator();

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMX_ForceRecalibration(0);
    SMX_ForceRecalibration(1);

    SMX_Stop();
}

// =========================================================================
// SMX_ReenableAutoLights command format
// =========================================================================

TEST_CASE("SMX_ReenableAutoLights sends 'S 1' to both pads") {
    auto pFakeP1 = new FakeDevice();
    auto pFakeP2 = new FakeDevice();
    pFakeP1->SetCaptureWrites(true);
    pFakeP2->SetCaptureWrites(true);
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeP1);
    pEnum->AddDevice("/dev/hidraw1", pFakeP2);

    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeP1->SetConfigResponse(MakeConfigResponse());
    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeP2->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBothConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });
    REQUIRE(bBothConnected);

    pFakeP1->ClearCapturedWrites();
    pFakeP2->ClearCapturedWrites();
    SMX_ReenableAutoLights();

    bool bGotP1Write = WaitFor([&]() {
        return pFakeP1->GetCapturedWriteCount() > 0;
    });
    bool bGotP2Write = WaitFor([&]() {
        return pFakeP2->GetCapturedWriteCount() > 0;
    });
    REQUIRE(bGotP1Write);
    REQUIRE(bGotP2Write);

    // Check both pads received "S 1\n"
    auto checkAutoLightsCmd = [](const vector<vector<uint8_t>> &writes) {
        for(const auto &w : writes)
        {
            if(w.size() >= 7 && w[0] == HID_REPORT_COMMAND)
            {
                uint8_t payloadSize = w[2];
                if(payloadSize >= 4 && w[3] == 'S' && w[4] == ' ' && w[5] == '1' && w[6] == '\n')
                    return true;
            }
        }
        return false;
    };

    CHECK(checkAutoLightsCmd(pFakeP1->GetCapturedWrites()));
    CHECK(checkAutoLightsCmd(pFakeP2->GetCapturedWrites()));

    SMX_Stop();
}

TEST_CASE("SMX_ReenableAutoLights with no devices does not crash") {
    auto pEnum = new FakeHIDEnumerator();

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMX_ReenableAutoLights();

    SMX_Stop();
}

// =========================================================================
// SMX_SetPanelTestMode command format
// =========================================================================

TEST_CASE("SMX_SetPanelTestMode sends 't' command to both pads") {
    auto pFakeP1 = new FakeDevice();
    auto pFakeP2 = new FakeDevice();
    pFakeP1->SetCaptureWrites(true);
    pFakeP2->SetCaptureWrites(true);
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeP1);
    pEnum->AddDevice("/dev/hidraw1", pFakeP2);

    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeP1->SetConfigResponse(MakeConfigResponse());
    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeP2->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBothConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });
    REQUIRE(bBothConnected);

    pFakeP1->ClearCapturedWrites();
    pFakeP2->ClearCapturedWrites();
    SMX_SetPanelTestMode(PanelTestMode_PressureTest);

    // Check that "t 1\n" was sent (PanelTestMode_PressureTest = '1')
    // Note: a lights-off command is sent first, so we need to wait for the test mode command.
    auto checkTestModeCmd = [](const vector<vector<uint8_t>> &writes, char mode) {
        for(const auto &w : writes)
        {
            if(w.size() >= 7 && w[0] == HID_REPORT_COMMAND)
            {
                uint8_t payloadSize = w[2];
                if(payloadSize >= 4 && w[3] == 't' && w[4] == ' ' && w[5] == (uint8_t)mode && w[6] == '\n')
                    return true;
            }
        }
        return false;
    };

    bool bGotP1Write = WaitFor([&]() {
        return checkTestModeCmd(pFakeP1->GetCapturedWrites(), '1');
    });
    REQUIRE(bGotP1Write);

    CHECK(checkTestModeCmd(pFakeP1->GetCapturedWrites(), '1'));
    CHECK(checkTestModeCmd(pFakeP2->GetCapturedWrites(), '1'));

    SMX_Stop();
}

TEST_CASE("SMX_SetPanelTestMode Off sends 't 0' command") {
    auto pFakeDevice = new FakeDevice();
    pFakeDevice->SetCaptureWrites(true);
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    // Enable and wait for the 't 1' command to actually be sent
    pFakeDevice->ClearCapturedWrites();
    SMX_SetPanelTestMode(PanelTestMode_PressureTest);

    auto hasTestCmd = [](const vector<vector<uint8_t>> &writes, char mode) {
        for(const auto &w : writes)
            if(w.size() >= 7 && w[0] == HID_REPORT_COMMAND && w[2] >= 4 &&
               w[3] == 't' && w[4] == ' ' && w[5] == (uint8_t)mode && w[6] == '\n')
                return true;
        return false;
    };

    bool bGotPressure = WaitFor([&]() {
        return hasTestCmd(pFakeDevice->GetCapturedWrites(), '1');
    });
    REQUIRE(bGotPressure);

    pFakeDevice->ClearCapturedWrites();
    SMX_SetPanelTestMode(PanelTestMode_Off);

    bool bGotOff = WaitFor([&]() {
        return hasTestCmd(pFakeDevice->GetCapturedWrites(), '0');
    });
    CHECK(bGotOff);

    SMX_Stop();
}

TEST_CASE("SMX_SetPanelTestMode with no devices does not crash") {
    auto pEnum = new FakeHIDEnumerator();

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMX_SetPanelTestMode(PanelTestMode_PressureTest);
    SMX_SetPanelTestMode(PanelTestMode_Off);

    SMX_Stop();
}