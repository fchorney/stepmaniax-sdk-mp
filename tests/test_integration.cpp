#include <doctest/doctest.h>
#include "SMX.h"
#include "SMXDeviceConnection.h"
#include "SMXHIDInterface.h"
#include "SMXHIDRecorder.h"

#include <atomic>
#include <chrono>
#include <cmath>
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

// --- Integration tests requiring real SMX hardware ---
//
// These tests are gated behind BUILD_INTEGRATION_TESTS in CMake.
// They require a physical StepManiaX pad connected via USB.
//
// Usage:
//   cmake .. -DBUILD_INTEGRATION_TESTS=ON
//   make smx-integration-tests
//   ./smx-integration-tests
//
// To record HID traffic for replay regression tests:
//   SMX_CAPTURE_DIR=capture ./smx-integration-tests
//
// This records into p1_solo/, p2_solo/, or both_pads/ subdirectories
// based on detected hardware. These captures are used by test_replay.cpp.

// --- Helper: get capture directory from environment ---

static string GetCaptureDir()
{
    const char *pDir = getenv("SMX_CAPTURE_DIR");
    return pDir ? string(pDir) : string();
}

// --- Helper: determine capture subdirectory based on connected hardware ---

static string GetCaptureSubDir(const vector<HIDDeviceInfo> &devices)
{
    // We need to connect to determine P1/P2, but for directory naming
    // we use device count: 1 device = solo, 2 = both_pads.
    // The actual P1/P2 determination happens after connection.
    if(devices.size() >= 2)
        return "both_pads";
    return "";  // determined after connection
}

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

// --- Helper: connect with optional recording to a subdirectory ---

static bool StartWithRecording(const string &sSubDir, atomic<int> &iConnected, int iExpected)
{
    unique_ptr<IHIDEnumerator> pEnumerator;
    string sCaptureDir = GetCaptureDir();
    if(!sCaptureDir.empty())
    {
        string sDir = sCaptureDir + "/" + sSubDir;
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sDir));
    }
    else
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }

    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                static_cast<atomic<int>*>(pUser)->fetch_add(1);
        },
        &iConnected, std::move(pEnumerator));

    return WaitFor([&]() { return iConnected.load() >= iExpected; }, 5000);
}

static int DetectHardware()
{
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();
    return static_cast<int>(devices.size());
}

TEST_CASE("Real hardware: connection and device info")
{
    int iDeviceCount = DetectHardware();
    if(iDeviceCount == 0) { MESSAGE("No SMX hardware detected, skipping"); return; }

    atomic<int> iConnected{0};
    REQUIRE(StartWithRecording("connection", iConnected, iDeviceCount));

    int iConnectedCount = 0;
    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(!info.m_bConnected) continue;
        iConnectedCount++;
        CHECK(info.m_iFirmwareVersion > 0);
        MESSAGE("Slot ", i, ": fw=", info.m_iFirmwareVersion,
                " p2=", info.m_bIsPlayer2,
                " serial=", info.m_bHasSerialNumber ? info.m_Serial : "(none)");
    }
    CHECK(iConnectedCount == iDeviceCount);

    SMX_Stop();
}

TEST_CASE("Real hardware: force recalibration")
{
    int iDeviceCount = DetectHardware();
    if(iDeviceCount == 0) { MESSAGE("No SMX hardware detected, skipping"); return; }

    atomic<int> iConnected{0};
    REQUIRE(StartWithRecording("force_recalibration", iConnected, iDeviceCount));

    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected)
        {
            SMX_ForceRecalibration(i);
            MESSAGE("Sent force recalibration to pad ", i);
        }
    }
    this_thread::sleep_for(chrono::milliseconds(500));

    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected)
            MESSAGE("Pad ", i, " still connected after recalibration");
    }

    SMX_Stop();
}

TEST_CASE("Real hardware: panel test mode")
{
    int iDeviceCount = DetectHardware();
    if(iDeviceCount == 0) { MESSAGE("No SMX hardware detected, skipping"); return; }

    atomic<int> iConnected{0};
    REQUIRE(StartWithRecording("panel_test_mode", iConnected, iDeviceCount));

    SMX_SetPanelTestMode(PanelTestMode_PressureTest);
    MESSAGE("Enabled pressure test mode");
    this_thread::sleep_for(chrono::seconds(2));

    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected)
            MESSAGE("Pad ", i, " still connected during test mode");
    }

    SMX_SetPanelTestMode(PanelTestMode_Off);
    MESSAGE("Disabled panel test mode");
    this_thread::sleep_for(chrono::milliseconds(200));

    SMX_Stop();
}

TEST_CASE("Real hardware: re-enable auto lights")
{
    int iDeviceCount = DetectHardware();
    if(iDeviceCount == 0) { MESSAGE("No SMX hardware detected, skipping"); return; }

    atomic<int> iConnected{0};
    REQUIRE(StartWithRecording("reenable_auto_lights", iConnected, iDeviceCount));

    SMX_ReenableAutoLights();
    MESSAGE("Sent re-enable auto lights");
    this_thread::sleep_for(chrono::milliseconds(500));

    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected)
            MESSAGE("Pad ", i, " still connected after re-enable auto lights");
    }

    SMX_Stop();
}

TEST_CASE("Real hardware: input state reads")
{
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    struct CallbackData {
        atomic<int> iConnected{0};
        atomic<int> iPacketCount{0};
    } data;

    int iExpected = static_cast<int>(devices.size());

    SMX_Start(
        [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
            auto *d = static_cast<CallbackData *>(pUser);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                d->iConnected.fetch_add(1);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_InputState))
                d->iPacketCount.fetch_add(1, memory_order_relaxed);
        },
        &data);

    SMX_SetInputStateMode(true);  // fire on every packet

    REQUIRE(WaitFor([&]() { return data.iConnected.load() >= iExpected; }, 5000));

    MESSAGE("Connected ", data.iConnected.load(), " pad(s)");

    // Measure USB input packet rate for 1 second at default polling rate (1000us).
    data.iPacketCount = 0;
    this_thread::sleep_for(chrono::seconds(1));
    int iPacketsDefault = data.iPacketCount.load();

    MESSAGE("Default (1000us): ", iPacketsDefault, " input packets/sec");
    CHECK(iPacketsDefault >= 10);

    // Measure again at 500us polling rate
    SMX_SetPollingRate(50, 500);
    data.iPacketCount = 0;
    this_thread::sleep_for(chrono::seconds(1));
    int iPacketsFast = data.iPacketCount.load();

    MESSAGE("Fast (500us): ", iPacketsFast, " input packets/sec");
    CHECK(iPacketsFast >= 10);

    SMX_Stop();
}

TEST_CASE("Real hardware: factory reset restores defaults")
{
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    // Set up enumerator with optional recording
    unique_ptr<IHIDEnumerator> pEnumerator;
    string sCaptureDir = GetCaptureDir();
    if(!sCaptureDir.empty())
    {
        string sSubDir = sCaptureDir + "/factory_reset";
        MESSAGE("Recording HID traffic to: ", sSubDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sSubDir));
    }
    else
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }

    atomic<int> iConnected{0};
    atomic<int> iConfigUpdated{0};

    struct CbData { atomic<int> *pConnected; atomic<int> *pConfigUpdated; };
    CbData cbData{&iConnected, &iConfigUpdated};

    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            auto *d = static_cast<CbData*>(pUser);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                d->pConnected->fetch_add(1);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_ConfigUpdated))
                d->pConfigUpdated->fetch_add(1);
        },
        &cbData, std::move(pEnumerator));

    REQUIRE(WaitFor([&]() { return iConnected.load() >= 1; }, 5000));

    // Find a connected pad
    int iPad = -1;
    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected) { iPad = i; break; }
    }
    REQUIRE(iPad >= 0);

    // Save original config
    SMXConfig originalConfig = {};
    REQUIRE(SMX_GetConfig(iPad, &originalConfig));
    MESSAGE("Original panelDebounceMicroseconds: ", originalConfig.panelDebounceMicroseconds);

    // Set a non-default value
    SMXConfig modifiedConfig = originalConfig;
    modifiedConfig.panelDebounceMicroseconds = 9999;
    int iCountBefore = iConfigUpdated.load();
    SMX_SetConfig(iPad, &modifiedConfig);
    REQUIRE(WaitFor([&]() { return iConfigUpdated.load() > iCountBefore; }, 5000));

    // Verify the modification took
    SMXConfig readBack = {};
    REQUIRE(SMX_GetConfig(iPad, &readBack));
    REQUIRE(readBack.panelDebounceMicroseconds == 9999);

    // Factory reset
    iCountBefore = iConfigUpdated.load();
    SMX_FactoryReset(iPad);
    REQUIRE(WaitFor([&]() { return iConfigUpdated.load() > iCountBefore; }, 5000));

    // Verify config was reset (should no longer be 9999)
    SMXConfig resetConfig = {};
    REQUIRE(SMX_GetConfig(iPad, &resetConfig));
    CHECK(resetConfig.panelDebounceMicroseconds != 9999);
    MESSAGE("After factory reset panelDebounceMicroseconds: ", resetConfig.panelDebounceMicroseconds);

    // Restore original config
    iCountBefore = iConfigUpdated.load();
    SMX_SetConfig(iPad, &originalConfig);
    WaitFor([&]() { return iConfigUpdated.load() > iCountBefore; }, 5000);

    SMX_Stop();
}

TEST_CASE("Real hardware: config get/set round-trip")
{
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    // Set up enumerator with optional recording
    unique_ptr<IHIDEnumerator> pEnumerator;
    string sCaptureDir = GetCaptureDir();
    if(!sCaptureDir.empty())
    {
        string sSubDir = sCaptureDir + "/config_get_set";
        MESSAGE("Recording HID traffic to: ", sSubDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sSubDir));
    }
    else
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }

    atomic<int> iConnected{0};
    atomic<int> iConfigUpdated{0};

    struct CbData { atomic<int> *pConnected; atomic<int> *pConfigUpdated; };
    CbData cbData{&iConnected, &iConfigUpdated};

    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            auto *d = static_cast<CbData*>(pUser);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                d->pConnected->fetch_add(1);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_ConfigUpdated))
                d->pConfigUpdated->fetch_add(1);
        },
        &cbData, std::move(pEnumerator));

    REQUIRE(WaitFor([&]() { return iConnected.load() >= 1; }, 5000));

    // Find a connected pad
    int iPad = -1;
    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected) { iPad = i; break; }
    }
    REQUIRE(iPad >= 0);

    // Get original config
    SMXConfig originalConfig = {};
    REQUIRE(SMX_GetConfig(iPad, &originalConfig));
    MESSAGE("Original panelDebounceMicroseconds: ", originalConfig.panelDebounceMicroseconds);

    // Modify one value
    SMXConfig modifiedConfig = originalConfig;
    uint16_t iNewValue = (originalConfig.panelDebounceMicroseconds == 4000) ? 5000 : 4000;
    modifiedConfig.panelDebounceMicroseconds = iNewValue;

    // Write modified config
    int iCountBefore = iConfigUpdated.load();
    SMX_SetConfig(iPad, &modifiedConfig);

    // Wait for ConfigUpdated callback (read-back verification)
    bool bGotUpdate = WaitFor([&]() {
        return iConfigUpdated.load() > iCountBefore;
    }, 5000);
    CHECK(bGotUpdate);

    // Verify the change took effect
    SMXConfig readBack = {};
    REQUIRE(SMX_GetConfig(iPad, &readBack));
    CHECK(readBack.panelDebounceMicroseconds == iNewValue);
    MESSAGE("Read back panelDebounceMicroseconds: ", readBack.panelDebounceMicroseconds);

    // Restore original config
    iCountBefore = iConfigUpdated.load();
    SMX_SetConfig(iPad, &originalConfig);
    WaitFor([&]() { return iConfigUpdated.load() > iCountBefore; }, 5000);

    // Verify restore
    SMXConfig restored = {};
    REQUIRE(SMX_GetConfig(iPad, &restored));
    CHECK(restored.panelDebounceMicroseconds == originalConfig.panelDebounceMicroseconds);
    MESSAGE("Restored panelDebounceMicroseconds: ", restored.panelDebounceMicroseconds);

    SMX_Stop();
}

TEST_CASE("Real hardware: platform lights set red then blue")
{
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    // Set up enumerator with optional recording
    unique_ptr<IHIDEnumerator> pEnumerator;
    string sCaptureDir = GetCaptureDir();
    if(!sCaptureDir.empty())
    {
        string sSubDir = sCaptureDir + "/platform_lights";
        MESSAGE("Recording HID traffic to: ", sSubDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sSubDir));
    }
    else
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }

    atomic<int> iConnected{0};
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                static_cast<atomic<int>*>(pUser)->fetch_add(1);
        },
        &iConnected, std::move(pEnumerator));

    REQUIRE(WaitFor([&]() { return iConnected.load() >= 1; }, 5000));

    // Verify at least one pad has fw >= 4
    bool bSupported = false;
    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected && info.m_iFirmwareVersion >= 4)
            bSupported = true;
    }
    if(!bSupported)
    {
        MESSAGE("No pad with firmware v4+ detected, skipping platform lights test");
        SMX_Stop();
        return;
    }

    // Set all LEDs to red
    char lightData[88 * 3] = {};
    for(int i = 0; i < 88; i++)
    {
        lightData[i * 3 + 0] = static_cast<char>(255); // R
        lightData[i * 3 + 1] = 0;                      // G
        lightData[i * 3 + 2] = 0;                      // B
    }
    SMX_SetPlatformLights(lightData);
    MESSAGE("Set platform lights to RED");
    this_thread::sleep_for(chrono::seconds(2));

    // Set all LEDs to blue
    for(int i = 0; i < 88; i++)
    {
        lightData[i * 3 + 0] = 0;                      // R
        lightData[i * 3 + 1] = 0;                      // G
        lightData[i * 3 + 2] = static_cast<char>(255); // B
    }
    SMX_SetPlatformLights(lightData);
    MESSAGE("Set platform lights to BLUE");
    this_thread::sleep_for(chrono::seconds(2));

    // Re-enable auto lights to restore normal behavior
    SMX_ReenableAutoLights();
    MESSAGE("Re-enabled auto lights");
    this_thread::sleep_for(chrono::milliseconds(200));

    SMX_Stop();
}

TEST_CASE("Real hardware: sensor test mode all modes")
{
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    // Set up enumerator with optional recording
    unique_ptr<IHIDEnumerator> pEnumerator;
    string sCaptureDir = GetCaptureDir();
    if(!sCaptureDir.empty())
    {
        string sSubDir = sCaptureDir + "/sensor_test_mode";
        MESSAGE("Recording HID traffic to: ", sSubDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sSubDir));
    }
    else
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }

    atomic<int> iConnected{0};
    atomic<int> iTestDataReceived{0};

    struct CbData { atomic<int> *pConnected; atomic<int> *pTestData; };
    CbData cbData{&iConnected, &iTestDataReceived};

    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            auto *d = static_cast<CbData*>(pUser);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                d->pConnected->fetch_add(1);
            if(SMX_REASON_IS(reason, SMXUpdateCallback_SensorTestData))
                d->pTestData->fetch_add(1);
        },
        &cbData, std::move(pEnumerator));

    REQUIRE(WaitFor([&]() { return iConnected.load() >= 1; }, 5000));

    // Find a connected pad
    int iPad = -1;
    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected) { iPad = i; break; }
    }
    REQUIRE(iPad >= 0);

    SensorTestMode modes[] = {
        SensorTestMode_UncalibratedValues,
        SensorTestMode_CalibratedValues,
        SensorTestMode_Noise,
        SensorTestMode_Tare,
    };
    const char *modeNames[] = { "Uncalibrated", "Calibrated", "Noise", "Tare" };

    for(int m = 0; m < 4; m++)
    {
        CAPTURE(modeNames[m]);

        iTestDataReceived = 0;
        SMX_SetTestMode(iPad, modes[m]);

        // Wait for at least one sensor data callback
        bool bGotData = WaitFor([&]() {
            return iTestDataReceived.load() >= 1;
        }, 5000);
        CHECK(bGotData);

        if(bGotData)
        {
            SMXSensorTestModeData data = {};
            REQUIRE(SMX_GetTestData(iPad, &data));

            // At least some panels should have responded
            int iPanelsWithData = 0;
            for(int p = 0; p < 9; p++)
                if(data.bHaveDataFromPanel[p])
                    iPanelsWithData++;

            CHECK(iPanelsWithData > 0);
            MESSAGE(modeNames[m], ": ", iPanelsWithData, " panels responded");

            // Print sensor values for first panel with data
            for(int p = 0; p < 9; p++)
            {
                if(!data.bHaveDataFromPanel[p])
                    continue;
                MESSAGE("  Panel ", p, " sensors: ",
                    data.sensorLevel[p][0], ", ",
                    data.sensorLevel[p][1], ", ",
                    data.sensorLevel[p][2], ", ",
                    data.sensorLevel[p][3]);
                break;
            }
        }

        SMX_SetTestMode(iPad, SensorTestMode_Off);
        this_thread::sleep_for(chrono::milliseconds(200));
    }

    SMX_Stop();
}

TEST_CASE("Real hardware: panel lights rainbow sweep at 30 FPS")
{
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    // Set up enumerator with optional recording
    unique_ptr<IHIDEnumerator> pEnumerator;
    string sCaptureDir = GetCaptureDir();
    if(!sCaptureDir.empty())
    {
        string sSubDir = sCaptureDir + "/panel_lights";
        MESSAGE("Recording HID traffic to: ", sSubDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sSubDir));
    }
    else
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }

    atomic<int> iConnected{0};
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                static_cast<atomic<int>*>(pUser)->fetch_add(1);
        },
        &iConnected, std::move(pEnumerator));

    int iExpected = static_cast<int>(devices.size());
    REQUIRE(WaitFor([&]() { return iConnected.load() >= iExpected; }, 5000));

    MESSAGE("Connected ", iConnected.load(), " pad(s), running 3-second rainbow sweep");

    // HSV to RGB helper (hue 0-360, returns r/g/b 0-255)
    auto hsvToRgb = [](float h, float s, float v, uint8_t &r, uint8_t &g, uint8_t &b) {
        float c = v * s;
        float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
        float m = v - c;
        float rf = 0, gf = 0, bf = 0;
        if(h < 60)       { rf = c; gf = x; }
        else if(h < 120) { rf = x; gf = c; }
        else if(h < 180) { gf = c; bf = x; }
        else if(h < 240) { gf = x; bf = c; }
        else if(h < 300) { rf = x; bf = c; }
        else             { rf = c; bf = x; }
        r = uint8_t((rf + m) * 255.0f);
        g = uint8_t((gf + m) * 255.0f);
        b = uint8_t((bf + m) * 255.0f);
    };

    // Run a rainbow sweep across all panels for 3 seconds at 30 FPS.
    // Each panel gets a different hue offset so the rainbow "moves" across the pad.
    const int iDurationMs = 3000;
    const int iFrameMs = 33; // ~30 FPS
    const int iLedsPerPanel = 25;
    const int iTotalBytes = 2 * 9 * iLedsPerPanel * 3; // 1350

    auto tStart = chrono::steady_clock::now();
    int iFrameCount = 0;

    while(true)
    {
        auto tNow = chrono::steady_clock::now();
        int iElapsedMs = static_cast<int>(
            chrono::duration_cast<chrono::milliseconds>(tNow - tStart).count());
        if(iElapsedMs >= iDurationMs)
            break;

        float fProgress = static_cast<float>(iElapsedMs) / iDurationMs; // 0.0 to 1.0

        vector<char> lightData(iTotalBytes, 0);
        for(int iPad = 0; iPad < 2; iPad++)
        {
            for(int iPanel = 0; iPanel < 9; iPanel++)
            {
                // Each panel gets a hue offset based on its position + time
                float fBaseHue = fmodf(fProgress * 360.0f + iPanel * 40.0f + iPad * 180.0f, 360.0f);

                for(int iLed = 0; iLed < iLedsPerPanel; iLed++)
                {
                    // Slight hue variation per LED within a panel
                    float fHue = fmodf(fBaseHue + iLed * 2.0f, 360.0f);
                    uint8_t r, g, b;
                    hsvToRgb(fHue, 1.0f, 1.0f, r, g, b);

                    int iOffset = (iPad * 9 * iLedsPerPanel + iPanel * iLedsPerPanel + iLed) * 3;
                    lightData[iOffset + 0] = static_cast<char>(r);
                    lightData[iOffset + 1] = static_cast<char>(g);
                    lightData[iOffset + 2] = static_cast<char>(b);
                }
            }
        }

        SMX_SetLights2(lightData.data(), iTotalBytes);
        iFrameCount++;

        // Sleep to maintain ~30 FPS
        auto tFrameEnd = tNow + chrono::milliseconds(iFrameMs);
        this_thread::sleep_until(tFrameEnd);
    }

    MESSAGE("Sent ", iFrameCount, " frames in 3 seconds (~", iFrameCount / 3, " FPS)");
    CHECK(iFrameCount >= 80); // Should be ~90 frames at 30 FPS

    // Re-enable auto lights to restore normal behavior
    SMX_ReenableAutoLights();
    this_thread::sleep_for(chrono::milliseconds(500));

    // Verify still connected
    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected)
            MESSAGE("Pad ", i, " still connected after lights test");
    }

    SMX_Stop();
}

TEST_CASE("Real hardware: panel animation playback from GIF")
{
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    // Set up enumerator with optional recording
    unique_ptr<IHIDEnumerator> pEnumerator;
    string sCaptureDir = GetCaptureDir();
    if(!sCaptureDir.empty())
    {
        string sSubDir = sCaptureDir + "/panel_animation";
        MESSAGE("Recording HID traffic to: ", sSubDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sSubDir));
    }
    else
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }

    atomic<int> iConnected{0};
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                static_cast<atomic<int>*>(pUser)->fetch_add(1);
        },
        &iConnected, std::move(pEnumerator));

    int iExpected = static_cast<int>(devices.size());
    REQUIRE(WaitFor([&]() { return iConnected.load() >= iExpected; }, 5000));

    MESSAGE("Connected ", iConnected.load(), " pad(s)");

    // --- Generate a 23x24 animated GIF with a color cycle ---
    // HSV helper
    auto hsvToRgb = [](float h, uint8_t &r, uint8_t &g, uint8_t &b) {
        float c = 1.0f, x = 1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f);
        float rf = 0, gf = 0, bf = 0;
        if(h < 60)       { rf = c; gf = x; }
        else if(h < 120) { rf = x; gf = c; }
        else if(h < 180) { gf = c; bf = x; }
        else if(h < 240) { gf = x; bf = c; }
        else if(h < 300) { rf = x; bf = c; }
        else             { rf = c; bf = x; }
        r = uint8_t(rf * 255); g = uint8_t(gf * 255); b = uint8_t(bf * 255);
    };

    // Build a minimal GIF89a with 6 frames (one per 60° hue step), 23x24, 100ms delay.
    // Each frame is a solid color across all panels.
    const int W = 23, H = 24;
    const int numFrames = 6;

    vector<uint8_t> gif;
    auto pushByte = [&](uint8_t b) { gif.push_back(b); };
    auto pushLE16 = [&](uint16_t v) { gif.push_back(v & 0xFF); gif.push_back(v >> 8); };

    // Header + LSD
    const char *hdr = "GIF89a";
    gif.insert(gif.end(), hdr, hdr + 6);
    pushLE16(W); pushLE16(H);
    // GCT: 8 colors (3 bits), flag set
    pushByte(0x82); // GCT flag + 3-bit color res + 8 colors (2^(2+1)=8)
    pushByte(0); pushByte(0);

    // Global Color Table: 8 entries (we'll use indices 0-5 for our 6 hues)
    for(int i = 0; i < 8; i++)
    {
        uint8_t r, g, b;
        hsvToRgb(i * 60.0f, r, g, b);
        pushByte(r); pushByte(g); pushByte(b);
    }

    // NETSCAPE looping extension
    pushByte(0x21); pushByte(0xFF); pushByte(11);
    const char *ns = "NETSCAPE2.0";
    gif.insert(gif.end(), ns, ns + 11);
    pushByte(3); pushByte(1); pushLE16(0); pushByte(0);

    // Frames
    for(int f = 0; f < numFrames; f++)
    {
        // GCE: 100ms delay
        pushByte(0x21); pushByte(0xF9); pushByte(4);
        pushByte(0); pushLE16(10); pushByte(0); pushByte(0);

        // Image descriptor
        pushByte(0x2C);
        pushLE16(0); pushLE16(0); pushLE16(W); pushLE16(H);
        pushByte(0);

        // LZW: min code size 3 (for 8-color palette)
        pushByte(3);
        int totalPixels = W * H;
        // clear=8, end=9, initial code size=4 bits
        // Emit clear code frequently to keep code size at 4 bits (avoids
        // needing to handle LZW dictionary growth in this simple encoder).
        vector<uint8_t> lzw;
        int bits = 0, bitCount = 0, codeSize = 4;
        auto emit = [&](int code) {
            bits |= (code << bitCount);
            bitCount += codeSize;
            while(bitCount >= 8) { lzw.push_back(bits & 0xFF); bits >>= 8; bitCount -= 8; }
        };
        emit(8); // clear
        int sinceLastClear = 0;
        for(int i = 0; i < totalPixels; i++)
        {
            emit(f);
            sinceLastClear++;
            // Reset before dictionary grows past code size 4 (max 6 entries before needing 5 bits)
            if(sinceLastClear >= 5)
            {
                emit(8); // clear — resets dictionary
                sinceLastClear = 0;
            }
        }
        emit(9); // end
        if(bitCount > 0) lzw.push_back(bits & 0xFF);

        for(size_t i = 0; i < lzw.size(); ) {
            size_t bs = min((size_t)255, lzw.size() - i);
            pushByte((uint8_t)bs);
            gif.insert(gif.end(), lzw.begin() + i, lzw.begin() + i + bs);
            i += bs;
        }
        pushByte(0);
    }
    pushByte(0x3B); // trailer

    MESSAGE("Generated ", numFrames, "-frame animated GIF (", gif.size(), " bytes)");

    // --- Load and play the animation ---
    const char *error = nullptr;
    bool bLoaded = SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(),
                                             0, SMX_LightsType_Released, &error);
    REQUIRE(bLoaded);
    MESSAGE("Loaded released animation for pad 0");

    // Also load for pad 1 if connected
    SMXInfo info1;
    SMX_GetInfo(1, &info1);
    if(info1.m_bConnected)
    {
        bLoaded = SMX_LightsAnimation_Load((const char*)gif.data(), gif.size(),
                                            1, SMX_LightsType_Released, &error);
        REQUIRE(bLoaded);
        MESSAGE("Loaded released animation for pad 1");
    }

    // Enable auto-animation and let it play for 3 seconds
    SMX_LightsAnimation_SetAuto(true);
    MESSAGE("Auto-animation enabled, playing for 3 seconds...");
    this_thread::sleep_for(chrono::seconds(3));

    // Verify still connected
    for(int i = 0; i < 2; i++)
    {
        SMXInfo info;
        SMX_GetInfo(i, &info);
        if(info.m_bConnected)
            MESSAGE("Pad ", i, " still connected during animation");
    }

    // Disable animation
    SMX_LightsAnimation_SetAuto(false);
    MESSAGE("Auto-animation disabled");

    // Re-enable auto lights to restore normal behavior
    SMX_ReenableAutoLights();
    this_thread::sleep_for(chrono::milliseconds(500));

    SMX_Stop();
}

TEST_CASE("Real hardware: animation upload to firmware")
{
    int iDeviceCount = DetectHardware();
    if(iDeviceCount == 0) { MESSAGE("No SMX hardware detected, skipping"); return; }

    atomic<int> iConnected{0};
    REQUIRE(StartWithRecording("animation_upload", iConnected, iDeviceCount));

    // Verify firmware v4+ (required for upload)
    SMXInfo info;
    SMX_GetInfo(0, &info);
    REQUIRE(info.m_bConnected);
    if(info.m_iFirmwareVersion < 4)
    {
        MESSAGE("Firmware v4+ required for animation upload, skipping (fw=", info.m_iFirmwareVersion, ")");
        SMX_Stop();
        return;
    }

    // --- Generate a simple 23x24 animated GIF (3 frames: red, green, blue) ---
    const int W = 23, H = 24;
    vector<uint8_t> gif;
    auto pb = [&](uint8_t b) { gif.push_back(b); };
    auto p16 = [&](uint16_t v) { gif.push_back(v & 0xFF); gif.push_back(v >> 8); };

    // Header + LSD with 8-color GCT
    const char *hdr = "GIF89a";
    gif.insert(gif.end(), hdr, hdr + 6);
    p16(W); p16(H);
    pb(0x82); pb(0); pb(0); // GCT flag, 8 colors
    uint8_t colors[8][3] = {{255,0,0},{0,255,0},{0,0,255},{0,0,0},{0,0,0},{0,0,0},{0,0,0},{0,0,0}};
    for(int i = 0; i < 8; i++) { pb(colors[i][0]); pb(colors[i][1]); pb(colors[i][2]); }

    // NETSCAPE loop
    pb(0x21); pb(0xFF); pb(11);
    const char *ns = "NETSCAPE2.0";
    gif.insert(gif.end(), ns, ns + 11);
    pb(3); pb(1); p16(0); pb(0);

    // 3 frames at 500ms each
    for(int f = 0; f < 3; f++)
    {
        pb(0x21); pb(0xF9); pb(4); pb(0); p16(50); pb(0); pb(0); // GCE: 500ms
        pb(0x2C); p16(0); p16(0); p16(W); p16(H); pb(0);
        pb(3); // LZW min code size
        int total = W * H;
        vector<uint8_t> lzw;
        int bits = 0, bc = 0, cs = 4;
        auto emit = [&](int code) { bits |= (code << bc); bc += cs; while(bc >= 8) { lzw.push_back(bits & 0xFF); bits >>= 8; bc -= 8; } };
        emit(8); // clear
        int since = 0;
        for(int i = 0; i < total; i++) { emit(f); since++; if(since >= 5) { emit(8); since = 0; } }
        emit(9); // end
        if(bc > 0) lzw.push_back(bits & 0xFF);
        for(size_t i = 0; i < lzw.size(); ) { size_t bs = min((size_t)255, lzw.size() - i); pb((uint8_t)bs); gif.insert(gif.end(), lzw.begin() + i, lzw.begin() + i + bs); i += bs; }
        pb(0);
    }
    pb(0x3B);

    MESSAGE("Generated 3-frame upload GIF (", gif.size(), " bytes)");

    // --- Prepare and upload ---
    const char *error = nullptr;
    bool bPrepared = SMX_LightsUpload_PrepareUpload((const char*)gif.data(), gif.size(),
                                                     0, SMX_LightsType_Released, &error);
    REQUIRE(bPrepared);
    MESSAGE("Prepared released animation for upload");

    // Begin upload with progress tracking
    struct UploadState {
        atomic<int> lastProgress{0};
        atomic<bool> complete{false};
    } state;

    SMX_LightsUpload_BeginUpload(0,
        [](int progress, void *pUser) {
            auto *s = static_cast<UploadState*>(pUser);
            s->lastProgress.store(progress);
            if(progress >= 100)
                s->complete.store(true);
        }, &state);

    // Wait for upload to complete (may take several seconds due to EEPROM delays)
    bool bCompleted = WaitFor([&]() { return state.complete.load(); }, 30000);
    CHECK(bCompleted);
    MESSAGE("Upload completed, final progress: ", state.lastProgress.load());

    // Give the firmware a moment to apply the animation
    this_thread::sleep_for(chrono::seconds(3));
    MESSAGE("Animation should be playing on pad (red -> green -> blue cycle)");

    // Verify still connected
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);

    // Restore normal behavior
    SMX_ReenableAutoLights();
    this_thread::sleep_for(chrono::milliseconds(500));
    MESSAGE("Re-enabled auto lights to restore normal behavior");

    SMX_Stop();
}
