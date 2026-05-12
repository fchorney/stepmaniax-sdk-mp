#include <doctest/doctest.h>
#include "SMX.h"
#include "SMXDeviceConnection.h"
#include "SMXHIDInterface.h"
#include "SMXHIDRecorder.h"
#include "SMXProtocolConstants.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace SMX;

// Declared in SMX.cpp (test-only, not exported from shared lib)
extern void SMX_StartWithEnumerator(SMXUpdateCallback callback, void *pUser,
                                     std::unique_ptr<SMX::IHIDEnumerator> pEnumerator);

// --- ReplayHIDEnumerator: serves ReplayHIDDevices from capture files ---

class ReplayHIDEnumerator : public IHIDEnumerator
{
public:
    void AddCapture(const string &sCaptureFile)
    {
        m_aCaptures.push_back(sCaptureFile);
    }

    void Init() override {}
    void Exit() override {}

    vector<HIDDeviceInfo> Enumerate(uint16_t, uint16_t) override
    {
        vector<HIDDeviceInfo> results;
        for(size_t i = 0; i < m_aCaptures.size(); i++)
        {
            HIDDeviceInfo info;
            info.sPath = "/replay/" + to_string(i);
            info.sProduct = SMX_USB_PRODUCT_STRING;
            results.push_back(info);
        }
        return results;
    }

    unique_ptr<IHIDDevice> Open(const string &path) override
    {
        size_t iIdx = static_cast<size_t>(stoi(path.substr(8)));
        if(iIdx >= m_aCaptures.size())
            return nullptr;
        auto pDev = new ReplayHIDDevice(m_aCaptures[iIdx]);
        m_aOpenedDevices.push_back(pDev);
        return unique_ptr<IHIDDevice>(pDev);
    }

    /// Returns raw pointers to opened replay devices for post-test verification.
    /// Only valid while the SDK is still running (devices are owned by the SDK).
    const vector<ReplayHIDDevice*> &GetOpenedDevices() const { return m_aOpenedDevices; }

private:
    vector<string> m_aCaptures;
    vector<ReplayHIDDevice*> m_aOpenedDevices;  // non-owning, for verification
};

// --- Helper: wait for condition with timeout ---

static bool WaitFor(function<bool()> cond, int iTimeoutMs = 5000)
{
    auto deadline = chrono::steady_clock::now() + chrono::milliseconds(iTimeoutMs);
    while(!cond())
    {
        if(chrono::steady_clock::now() >= deadline)
            return false;
        this_thread::sleep_for(chrono::milliseconds(10));
    }
    return true;
}

// --- Helper: check if a capture file exists ---

static bool CaptureExists(const string &sPath)
{
    ifstream f(sPath);
    return f.good();
}

// --- Helper: get capture path relative to source tree ---

#ifndef CAPTURE_DIR
#define CAPTURE_DIR "capture"
#endif

static string CapturePath(const string &sRelative)
{
    return string(CAPTURE_DIR) + "/" + sRelative;
}

// --- Helper: check if a specific command byte sequence appears in writes ---
// Writes are HID packets: [report_id=5][flags][size][payload...]

static bool WritesContainCommand(const vector<vector<uint8_t>> &writes, const string &sCmd)
{
    for(const auto &w : writes)
    {
        if(w.size() < 3 + sCmd.size() || w[0] != HID_REPORT_COMMAND)
            continue;
        uint8_t payloadSize = w[2];
        if(payloadSize < sCmd.size())
            continue;
        if(memcmp(&w[3], sCmd.data(), sCmd.size()) == 0)
            return true;
    }
    return false;
}

// --- Replay regression tests ---

TEST_CASE("Replay: connection")
{
    string sFile0 = CapturePath("connection/device_0.smxhid");
    if(!CaptureExists(sFile0))
    {
        MESSAGE("Capture not found: ", sFile0, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);

    string sFile1 = CapturePath("connection/device_1.smxhid");
    bool bHasSecondDevice = CaptureExists(sFile1);
    if(bHasSecondDevice)
        pEnum->AddCapture(sFile1);

    int iExpected = bHasSecondDevice ? 2 : 1;
    atomic<int> iConnectedCount{0};
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                static_cast<atomic<int>*>(pUser)->fetch_add(1);
        },
        &iConnectedCount, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return iConnectedCount >= iExpected; }, 5000));

    // Verify slot 0 is connected and is P1
    SMXInfo info0;
    SMX_GetInfo(0, &info0);
    CHECK(info0.m_bConnected);
    CHECK_FALSE(info0.m_bIsPlayer2);
    CHECK(info0.m_iFirmwareVersion > 0);
    MESSAGE("Slot 0: fw=", info0.m_iFirmwareVersion,
            " serial=", info0.m_bHasSerialNumber ? info0.m_Serial : "(none)");

    if(bHasSecondDevice)
    {
        SMXInfo info1;
        SMX_GetInfo(1, &info1);
        CHECK(info1.m_bConnected);
        CHECK(info1.m_bIsPlayer2);
        CHECK(info1.m_iFirmwareVersion > 0);
        MESSAGE("Slot 1: fw=", info1.m_iFirmwareVersion,
                " serial=", info1.m_bHasSerialNumber ? info1.m_Serial : "(none)");
    }

    // Verify config read command in capture
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetExpectedWrites(), "G"));

    SMX_Stop();
}

TEST_CASE("Replay: force recalibration command in capture")
{
    string sFile = CapturePath("force_recalibration/device_0.smxhid");
    if(!CaptureExists(sFile))
    {
        MESSAGE("Capture not found: ", sFile, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    SMXInfo info;
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);

    // Verify the capture contains the recalibration command ("C\n")
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetExpectedWrites(), string("C\n", 2)));
    MESSAGE("Force recalibration command verified in replay writes");

    SMX_Stop();
}

TEST_CASE("Replay: panel test mode command in capture")
{
    string sFile = CapturePath("panel_test_mode/device_0.smxhid");
    if(!CaptureExists(sFile))
    {
        MESSAGE("Capture not found: ", sFile, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    SMXInfo info;
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);

    // Verify the capture contains panel test mode commands from the original recording.
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetExpectedWrites(), string("t 1\n", 4)));
    CHECK(WritesContainCommand(devs[0]->GetExpectedWrites(), string("t 0\n", 4)));
    MESSAGE("Panel test mode commands verified in replay writes");

    SMX_Stop();
}

TEST_CASE("Replay: re-enable auto lights command in capture")
{
    string sFile = CapturePath("reenable_auto_lights/device_0.smxhid");
    if(!CaptureExists(sFile))
    {
        MESSAGE("Capture not found: ", sFile, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    SMXInfo info;
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);

    // Verify the capture contains the auto lights command ("S 1\n")
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetExpectedWrites(), string("S 1\n", 4)));
    MESSAGE("Re-enable auto lights command verified in replay writes");

    SMX_Stop();
}

TEST_CASE("Replay: factory reset")
{
    string sFile0 = CapturePath("factory_reset/device_0.smxhid");
    string sFile1 = CapturePath("factory_reset/device_1.smxhid");
    if(!CaptureExists(sFile0))
    {
        MESSAGE("Capture not found: ", sFile0, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);
    if(CaptureExists(sFile1))
        pEnum->AddCapture(sFile1);

    struct CbData { bool bConnected = false; int iConfigUpdated = 0; };
    CbData cbData;

    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            auto *d = static_cast<CbData*>(pUser);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                d->bConnected = true;
            if(SMX_REASON_IS(reason, SMXUpdateCallback_ConfigUpdated))
                d->iConfigUpdated++;
        },
        &cbData, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return cbData.bConnected; }, 5000));

    // Verify we can read config after connection
    SMXConfig cfg = {};
    CHECK(SMX_GetConfig(0, &cfg));
    MESSAGE("Factory reset replay: initial panelDebounceMicroseconds=", cfg.panelDebounceMicroseconds);

    // Verify the capture contains a write command ("W" for config set)
    // and a factory reset command ("f\n") on at least one device
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    bool bFoundWrite = false;
    bool bFoundReset = false;
    for(const auto *dev : devs)
    {
        if(WritesContainCommand(dev->GetExpectedWrites(), "W"))
            bFoundWrite = true;
        if(WritesContainCommand(dev->GetExpectedWrites(), string("f\n", 2)))
            bFoundReset = true;
    }
    CHECK(bFoundWrite);
    CHECK(bFoundReset);

    SMX_Stop();
}

TEST_CASE("Replay: config get/set")
{
    string sFile0 = CapturePath("config_get_set/device_0.smxhid");
    string sFile1 = CapturePath("config_get_set/device_1.smxhid");
    if(!CaptureExists(sFile0))
    {
        MESSAGE("Capture not found: ", sFile0, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);
    if(CaptureExists(sFile1))
        pEnum->AddCapture(sFile1);

    struct CbData { bool bConnected = false; int iConfigUpdated = 0; };
    CbData cbData;

    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            auto *d = static_cast<CbData*>(pUser);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                d->bConnected = true;
            if(SMX_REASON_IS(reason, SMXUpdateCallback_ConfigUpdated))
                d->iConfigUpdated++;
        },
        &cbData, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return cbData.bConnected; }, 5000));

    // Verify we can read config after connection
    SMXConfig cfg = {};
    CHECK(SMX_GetConfig(0, &cfg));
    MESSAGE("Config get/set replay: panelDebounceMicroseconds=", cfg.panelDebounceMicroseconds);

    // Verify the capture contains config write commands ("W") on at least one device
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    bool bFoundWrite = false;
    bool bFoundRead = false;
    for(const auto *dev : devs)
    {
        if(WritesContainCommand(dev->GetExpectedWrites(), "W"))
            bFoundWrite = true;
        if(WritesContainCommand(dev->GetExpectedWrites(), "G"))
            bFoundRead = true;
    }
    CHECK(bFoundWrite);
    CHECK(bFoundRead);

    SMX_Stop();
}

TEST_CASE("Replay: platform lights")
{
    string sFile0 = CapturePath("platform_lights/device_0.smxhid");
    string sFile1 = CapturePath("platform_lights/device_1.smxhid");
    if(!CaptureExists(sFile0))
    {
        MESSAGE("Capture not found: ", sFile0, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);
    if(CaptureExists(sFile1))
        pEnum->AddCapture(sFile1);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool*>(pUser) = true;
        },
        &bConnected, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    // Verify the capture contains 'L' (platform lights) commands
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    bool bFoundLightCmd = false;
    for(const auto *dev : devs)
    {
        if(WritesContainCommand(dev->GetExpectedWrites(), "L"))
        {
            bFoundLightCmd = true;
            break;
        }
    }
    CHECK(bFoundLightCmd);

    // Verify the capture also contains 'S' (re-enable auto lights)
    bool bFoundAutoLights = false;
    for(const auto *dev : devs)
    {
        if(WritesContainCommand(dev->GetExpectedWrites(), string("S 1\n", 4)))
        {
            bFoundAutoLights = true;
            break;
        }
    }
    CHECK(bFoundAutoLights);

    SMX_Stop();
}

TEST_CASE("Replay: sensor test mode")
{
    string sFile0 = CapturePath("sensor_test_mode/device_0.smxhid");
    string sFile1 = CapturePath("sensor_test_mode/device_1.smxhid");
    if(!CaptureExists(sFile0))
    {
        MESSAGE("Capture not found: ", sFile0, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);
    if(CaptureExists(sFile1))
        pEnum->AddCapture(sFile1);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool*>(pUser) = true;
        },
        &bConnected, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    // Verify the capture contains 'y' (sensor test mode) commands
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    bool bFoundTestCmd = false;
    for(const auto *dev : devs)
    {
        if(WritesContainCommand(dev->GetExpectedWrites(), "y"))
        {
            bFoundTestCmd = true;
            break;
        }
    }
    CHECK(bFoundTestCmd);

    SMX_Stop();
}

TEST_CASE("Replay: panel lights commands in capture")
{
    string sFile0 = CapturePath("panel_lights/device_0.smxhid");
    string sFile1 = CapturePath("panel_lights/device_1.smxhid");
    if(!CaptureExists(sFile0))
    {
        MESSAGE("Capture not found: ", sFile0, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);
    if(CaptureExists(sFile1))
        pEnum->AddCapture(sFile1);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);

    // Analyze the recorded writes from device 0
    auto &writes = devs[0]->GetExpectedWrites();

    // Extract all lights commands in order
    struct LightsCmd { char type; size_t payloadSize; size_t index; };
    vector<LightsCmd> lightsCmds;
    for(size_t i = 0; i < writes.size(); i++)
    {
        const auto &w = writes[i];
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
           (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 1)
        {
            char cmd = static_cast<char>(w[3]);
            if(cmd == '2' || cmd == '3' || cmd == '4')
            {
                // Reconstruct full command size from all packets of this command
                // For START_OF_COMMAND packets, payload size is in w[2]
                lightsCmds.push_back({cmd, w[2], i});
            }
        }
    }

    // Should have many lights commands (3 per frame, ~90 frames = ~270 commands)
    MESSAGE("Total lights commands in capture: ", lightsCmds.size());
    CHECK(lightsCmds.size() >= 30); // At least 10 full updates

    // Verify commands come in groups of 3: '4', '2', '3' (firmware v5)
    int iFullUpdates = 0;
    for(size_t i = 0; i + 2 < lightsCmds.size(); i += 3)
    {
        if(lightsCmds[i].type == '4' &&
           lightsCmds[i+1].type == '2' &&
           lightsCmds[i+2].type == '3')
        {
            iFullUpdates++;
        }
    }
    MESSAGE("Complete 4-2-3 update groups: ", iFullUpdates);
    CHECK(iFullUpdates >= 10);

    // Verify color scaling: no byte in the lights payload should exceed 170
    // (since all input values are scaled by 0.6666, max output is 255*0.6666 ≈ 170)
    int iMaxColorValue = 0;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
           (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 2)
        {
            char cmd = static_cast<char>(w[3]);
            if(cmd == '2' || cmd == '3' || cmd == '4')
            {
                // Check color bytes in the payload (skip the command byte itself)
                for(size_t j = 4; j < (size_t)(3 + w[2]); j++)
                {
                    int val = static_cast<uint8_t>(w[j]);
                    if(val > iMaxColorValue)
                        iMaxColorValue = val;
                }
            }
        }
    }
    MESSAGE("Max color value in lights data: ", iMaxColorValue);
    CHECK(iMaxColorValue <= 170);
    CHECK(iMaxColorValue > 0); // Should have some non-zero colors

    // If we have two devices, verify both got lights commands
    if(devs.size() >= 2)
    {
        auto &writes1 = devs[1]->GetExpectedWrites();
        bool bDev1HasLights = false;
        for(const auto &w : writes1)
        {
            if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
               (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 1)
            {
                char cmd = static_cast<char>(w[3]);
                if(cmd == '2' || cmd == '3' || cmd == '4')
                {
                    bDev1HasLights = true;
                    break;
                }
            }
        }
        CHECK(bDev1HasLights);
        MESSAGE("Device 1 also received lights commands");
    }

    SMX_Stop();
}

TEST_CASE("Replay: panel animation lights commands in capture")
{
    string sFile0 = CapturePath("panel_animation/device_0.smxhid");
    if(!CaptureExists(sFile0))
    {
        MESSAGE("Capture not found: ", sFile0, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);

    string sFile1 = CapturePath("panel_animation/device_1.smxhid");
    if(CaptureExists(sFile1))
        pEnum->AddCapture(sFile1);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    // The panel animation integration test plays a 6-frame GIF for 3 seconds.
    // Verify the capture contains lights commands from the animation playback.
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);

    auto &writes = devs[0]->GetExpectedWrites();
    int iLightsCmdCount = 0;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
           (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 1)
        {
            char cmd = static_cast<char>(w[3]);
            if(cmd == '2' || cmd == '3' || cmd == '4')
                iLightsCmdCount++;
        }
    }

    // Should have many lights commands from 3 seconds of animation at 30 FPS
    MESSAGE("Panel animation capture: ", iLightsCmdCount, " lights commands");
    CHECK(iLightsCmdCount >= 30);

    // Verify the "S 1\n" (re-enable auto lights) command is present at the end
    CHECK(WritesContainCommand(devs[0]->GetExpectedWrites(), string("S 1\n", 4)));

    SMX_Stop();
}

TEST_CASE("Replay: animation upload commands in capture")
{
    string sFile0 = CapturePath("animation_upload/device_0.smxhid");
    if(!CaptureExists(sFile0))
    {
        MESSAGE("Capture not found: ", sFile0, " — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);

    string sFile1 = CapturePath("animation_upload/device_1.smxhid");
    if(CaptureExists(sFile1))
        pEnum->AddCapture(sFile1);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    // Verify the capture contains upload ('m') and delay ('d') commands
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);

    auto &writes = devs[0]->GetExpectedWrites();
    bool bFoundUpload = false, bFoundDelay = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND &&
           (w[1] & PACKET_FLAG_START_OF_COMMAND) && w[2] >= 1)
        {
            char cmd = static_cast<char>(w[3]);
            if(cmd == 'm') bFoundUpload = true;
            if(cmd == 'd') bFoundDelay = true;
        }
    }

    CHECK(bFoundUpload);
    CHECK(bFoundDelay);

    MESSAGE("Animation upload commands verified: upload=", bFoundUpload, " delay=", bFoundDelay);

    SMX_Stop();
}
