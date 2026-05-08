#include <doctest/doctest.h>
#include "SMX.h"
#include "SMXDeviceConnection.h"
#include "SMXHIDInterface.h"
#include "SMXHIDRecorder.h"

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

TEST_CASE("Real hardware: comprehensive command test")
{
    // Check if hardware is present
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    int iDeviceCount = static_cast<int>(devices.size());
    MESSAGE("Found ", iDeviceCount, " SMX device(s)");

    // Determine capture subdirectory. For a single device, we need to connect
    // first to know if it's P1 or P2, so we defer directory creation.
    string sCaptureDir = GetCaptureDir();
    string sSubDir;
    if(!sCaptureDir.empty() && iDeviceCount >= 2)
        sSubDir = sCaptureDir + "/both_pads";

    // Create enumerator (optionally recording)
    unique_ptr<IHIDEnumerator> pEnumerator;
    if(!sSubDir.empty())
    {
        MESSAGE("Recording HID traffic to: ", sSubDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sSubDir));
    }
    else if(sCaptureDir.empty())
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }
    else
    {
        // Single device — we'll determine P1/P2 after connection.
        // Use a non-recording enumerator first to detect, then restart with recording.
        // Actually, just connect without recording, check P1/P2, stop, then reconnect with recording.
        pEnumerator = CreateHIDAPIEnumerator();
    }

    // For single-device + capture: detect P1/P2 first, then re-run with recording
    if(!sCaptureDir.empty() && iDeviceCount == 1 && sSubDir.empty())
    {
        atomic<int> iConnected{0};
        SMX_StartWithEnumerator(
            [](int, SMXUpdateCallbackReason reason, void *pUser) {
                if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                    static_cast<atomic<int>*>(pUser)->fetch_add(1);
            },
            &iConnected, std::move(pEnumerator));

        REQUIRE(WaitFor([&]() { return iConnected.load() >= 1; }, 5000));

        // Determine P1 or P2
        SMXInfo info0, info1;
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        bool bIsP2 = info1.m_bConnected && info1.m_bIsPlayer2;
        SMX_Stop();

        sSubDir = sCaptureDir + (bIsP2 ? "/p2_solo" : "/p1_solo");
        MESSAGE("Recording HID traffic to: ", sSubDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sSubDir));
    }

    // Start SDK
    atomic<int> iConnected{0};
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                static_cast<atomic<int>*>(pUser)->fetch_add(1);
        },
        &iConnected, std::move(pEnumerator));

    REQUIRE(WaitFor([&]() { return iConnected.load() >= iDeviceCount; }, 5000));
    this_thread::sleep_for(chrono::milliseconds(200));

    // --- Verify connection ---
    SUBCASE("connection") {
        int iConnectedCount = 0;
        for(int i = 0; i < 2; i++)
        {
            SMXInfo info;
            SMX_GetInfo(i, &info);
            if(!info.m_bConnected)
                continue;
            iConnectedCount++;
            CHECK(info.m_iFirmwareVersion > 0);
            MESSAGE("Slot ", i, ": fw=", info.m_iFirmwareVersion,
                    " p2=", info.m_bIsPlayer2,
                    " serial=", info.m_bHasSerialNumber ? info.m_Serial : "(none)");
        }
        CHECK(iConnectedCount == iDeviceCount);
    }

    // --- Force recalibration ---
    SUBCASE("force recalibration") {
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

        // Verify still connected
        for(int i = 0; i < 2; i++)
        {
            SMXInfo info;
            SMX_GetInfo(i, &info);
            if(info.m_bConnected)
                MESSAGE("Pad ", i, " still connected after recalibration");
        }
    }

    // --- Panel test mode ---
    SUBCASE("panel test mode") {
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
    }

    // --- Re-enable auto lights ---
    SUBCASE("re-enable auto lights") {
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
