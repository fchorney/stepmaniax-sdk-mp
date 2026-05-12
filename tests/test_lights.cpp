#include <doctest/doctest.h>
#include "test_helpers_manager.h"

#include <cstring>
#include <vector>

using namespace std;
using namespace SMX;
using namespace SMXTestHelpers;

// =========================================================================
// SMX_SetLights2 tests
// =========================================================================

TEST_CASE("SMX_SetLights2 rejects invalid sizes") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    SMXConfig cfg = {};
    cfg.masterVersion = 5;
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() {
        SMXInfo info = {};
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    }));

    pFakeDevice->ClearCapturedWrites();

    // Invalid size should not produce any writes
    char buf[100] = {};
    SMX_SetLights2(buf, 100);
    this_thread::sleep_for(chrono::milliseconds(100));
    CHECK(pFakeDevice->GetCapturedWriteCount() == 0);

    SMX_Stop();
}

TEST_CASE("SMX_SetLights2 sends commands for 1350-byte input on firmware v4+") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    SMXConfig cfg = {};
    cfg.masterVersion = 5;
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() {
        SMXInfo info = {};
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    }));

    pFakeDevice->ClearCapturedWrites();

    // Send 1350 bytes (25-LED mode, both pads)
    vector<char> lightData(1350, 0);
    // Set a recognizable value in pad 0, panel 0, LED 0 (R channel)
    lightData[0] = (char)255;
    SMX_SetLights2(lightData.data(), 1350);

    // Wait for all 3 commands to be sent. Each command spans multiple HID packets,
    // and the command queue is sequential (waits for ACK before sending next).
    // The '4' command is ~245 bytes (5 packets), '2' and '3' are ~218 bytes (4 packets each).
    // Total: ~13 HID writes for all 3 commands.
    bool bWritten = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() >= 12;
    });
    CHECK(bWritten);

    // Verify we got commands starting with '4', '2', '3'
    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFound4 = false, bFound2 = false, bFound3 = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
           (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 1)
        {
            char cmd = static_cast<char>(w[3]);
            if(cmd == '4') bFound4 = true;
            if(cmd == '2') bFound2 = true;
            if(cmd == '3') bFound3 = true;
        }
    }
    CHECK(bFound4);
    CHECK(bFound2);
    CHECK(bFound3);

    SMX_Stop();
}

TEST_CASE("SMX_SetLights2 does not send command '4' on firmware < v4") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    SMXConfig cfg = {};
    cfg.masterVersion = 3;
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 3));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() {
        SMXInfo info = {};
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    }));

    pFakeDevice->ClearCapturedWrites();

    // Send 864 bytes (16-LED mode)
    vector<char> lightData(864, 0);
    lightData[0] = (char)200;
    SMX_SetLights2(lightData.data(), 864);

    // Wait for commands — firmware <v4 uses delays so give it time
    this_thread::sleep_for(chrono::milliseconds(200));

    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFound4 = false, bFound2 = false, bFound3 = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
           (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 1)
        {
            char cmd = static_cast<char>(w[3]);
            if(cmd == '4') bFound4 = true;
            if(cmd == '2') bFound2 = true;
            if(cmd == '3') bFound3 = true;
        }
    }
    CHECK_FALSE(bFound4);
    CHECK(bFound2);
    CHECK(bFound3);

    SMX_Stop();
}

TEST_CASE("SMX_SetLights2 applies color scaling") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    SMXConfig cfg = {};
    cfg.masterVersion = 5;
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() {
        SMXInfo info = {};
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    }));

    pFakeDevice->ClearCapturedWrites();

    // Set all LEDs to 255 for pad 0 — after scaling should be ~170
    vector<char> lightData(1350, 0);
    // Fill pad 0 panel 0 LED 0 RGB with 255
    lightData[0] = (char)255;
    lightData[1] = (char)255;
    lightData[2] = (char)255;
    SMX_SetLights2(lightData.data(), 1350);

    bool bWritten = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() >= 3;
    });
    REQUIRE(bWritten);

    // Find the '2' command (top half) — LED 0 is in the top half
    auto writes = pFakeDevice->GetCapturedWrites();
    for(const auto &w : writes)
    {
        if(w.size() >= 7 && w[0] == HID_REPORT_COMMAND)
        {
            uint8_t flags = w[1];
            if(!(flags & PACKET_FLAG_START_OF_COMMAND))
                continue;
            if(w[2] < 4)
                continue;
            char cmd = static_cast<char>(w[3]);
            if(cmd == '2')
            {
                // First 3 bytes after the '2' command byte are the scaled RGB
                uint8_t r = w[4];
                uint8_t g = w[5];
                uint8_t b = w[6];
                // 255 * 0.6666 = ~169-170
                CHECK(r >= 169);
                CHECK(r <= 170);
                CHECK(g >= 169);
                CHECK(g <= 170);
                CHECK(b >= 169);
                CHECK(b <= 170);
                break;
            }
        }
    }

    SMX_Stop();
}

TEST_CASE("SMX_SetLights sends both pads together") {
    auto pFakeDevice0 = new FakeDevice();
    auto pFakeDevice1 = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice0);
    pEnum->AddDevice("/dev/hidraw1", pFakeDevice1);

    SMXConfig cfg = {};
    cfg.masterVersion = 5;
    pFakeDevice0->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice0->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice0->SetCaptureWrites(true);

    pFakeDevice1->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeDevice1->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice1->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() {
        SMXInfo info0 = {}, info1 = {};
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    }));

    pFakeDevice0->ClearCapturedWrites();
    pFakeDevice1->ClearCapturedWrites();

    // Send 1350 bytes — both pads should get commands
    vector<char> lightData(1350, (char)100);
    SMX_SetLights2(lightData.data(), 1350);

    bool bBothWritten = WaitFor([&]() {
        return pFakeDevice0->GetCapturedWriteCount() >= 12 &&
               pFakeDevice1->GetCapturedWriteCount() >= 12;
    });
    CHECK(bBothWritten);

    SMX_Stop();
}

TEST_CASE("SMX_SetLights is equivalent to SMX_SetLights2 with 864") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    SMXConfig cfg = {};
    cfg.masterVersion = 5;
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() {
        SMXInfo info = {};
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    }));

    pFakeDevice->ClearCapturedWrites();

    // Use the deprecated SMX_SetLights with 864 bytes
    char lightData[864] = {};
    lightData[0] = (char)150;
    SMX_SetLights(lightData);

    bool bWritten = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() >= 12;
    });
    CHECK(bWritten);

    // Should have sent '4', '2', '3' commands (firmware v5)
    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFound2 = false, bFound3 = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
           (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 1)
        {
            char cmd = static_cast<char>(w[3]);
            if(cmd == '2') bFound2 = true;
            if(cmd == '3') bFound3 = true;
        }
    }
    CHECK(bFound2);
    CHECK(bFound3);

    SMX_Stop();
}

TEST_CASE("SMX_SetLights2 does not send when panel test mode is active") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    SMXConfig cfg = {};
    cfg.masterVersion = 5;
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() {
        SMXInfo info = {};
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    }));

    // Enable panel test mode
    SMX_SetPanelTestMode(PanelTestMode_PressureTest);
    this_thread::sleep_for(chrono::milliseconds(100));

    pFakeDevice->ClearCapturedWrites();

    // Try to send lights — should be blocked
    vector<char> lightData(1350, (char)100);
    SMX_SetLights2(lightData.data(), 1350);
    this_thread::sleep_for(chrono::milliseconds(100));

    // Check that no lights commands ('2', '3', '4') were sent
    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFoundLightsCmd = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
           (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 1)
        {
            char cmd = static_cast<char>(w[3]);
            if(cmd == '2' || cmd == '3' || cmd == '4')
            {
                bFoundLightsCmd = true;
                break;
            }
        }
    }
    CHECK_FALSE(bFoundLightsCmd);

    SMX_Stop();
}

TEST_CASE("SMX_SetLights2 rate limits to 30 FPS") {
    auto pFakeDevice = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    SMXConfig cfg = {};
    cfg.masterVersion = 5;
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponsePackets(MakeFullConfigResponsePackets(cfg));
    pFakeDevice->SetCaptureWrites(true);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() {
        SMXInfo info = {};
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    }));

    pFakeDevice->ClearCapturedWrites();

    // Send lights rapidly — should only get one set of 3 commands queued
    vector<char> lightData(1350, 0);
    for(int i = 0; i < 10; i++)
    {
        lightData[0] = static_cast<char>(i);
        SMX_SetLights2(lightData.data(), 1350);
    }

    // Wait for the first batch to send
    bool bWritten = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() >= 12;
    });
    CHECK(bWritten);

    // Count how many distinct lights commands ('4', '2', '3') were sent.
    // With rate limiting, we should have at most one set (3 commands total),
    // not 10 sets (30 commands).
    this_thread::sleep_for(chrono::milliseconds(50));
    auto writes = pFakeDevice->GetCapturedWrites();
    int iLightsCmdCount = 0;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND)
        {
            uint8_t flags = w[1];
            if(flags & PACKET_FLAG_START_OF_COMMAND)
            {
                char cmd = static_cast<char>(w[3]);
                if(cmd == '2' || cmd == '3' || cmd == '4')
                    iLightsCmdCount++;
            }
        }
    }
    // Should have exactly 3 commands (one full update), not 30
    CHECK(iLightsCmdCount == 3);

    SMX_Stop();
}
