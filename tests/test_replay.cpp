#include <doctest/doctest.h>
#include "SMX.h"
#include "SMXDeviceConnection.h"
#include "SMXHIDInterface.h"
#include "SMXHIDRecorder.h"

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

TEST_CASE("Replay: P1 solo connection")
{
    string sFile = CapturePath("p1_solo/device_0.smxhid");
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
    CHECK_FALSE(info.m_bIsPlayer2);
    CHECK(info.m_iFirmwareVersion > 0);
    MESSAGE("P1 solo: fw=", info.m_iFirmwareVersion,
            " serial=", info.m_bHasSerialNumber ? info.m_Serial : "(none)");

    // Verify expected commands are in the capture (config read on activation)
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetExpectedWrites(), "G"));

    SMX_Stop();
}

TEST_CASE("Replay: P2 solo connection")
{
    string sFile = CapturePath("p2_solo/device_0.smxhid");
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
    SMX_GetInfo(1, &info);
    CHECK(info.m_bConnected);
    CHECK(info.m_bIsPlayer2);
    CHECK(info.m_iFirmwareVersion > 0);
    MESSAGE("P2 solo: fw=", info.m_iFirmwareVersion,
            " serial=", info.m_bHasSerialNumber ? info.m_Serial : "(none)");

    // Slot 0 should be empty
    SMXInfo info0;
    SMX_GetInfo(0, &info0);
    CHECK_FALSE(info0.m_bConnected);

    // Verify config read command in capture
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetExpectedWrites(), "G"));

    SMX_Stop();
}

TEST_CASE("Replay: both pads connection")
{
    string sFile0 = CapturePath("both_pads/device_0.smxhid");
    string sFile1 = CapturePath("both_pads/device_1.smxhid");
    if(!CaptureExists(sFile0) || !CaptureExists(sFile1))
    {
        MESSAGE("Captures not found — skipping");
        return;
    }

    auto pEnum = new ReplayHIDEnumerator();
    pEnum->AddCapture(sFile0);
    pEnum->AddCapture(sFile1);

    int iConnectedCount = 0;
    SMX_StartWithEnumerator(
        [](int, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                (*static_cast<int *>(pUser))++;
        },
        &iConnectedCount, unique_ptr<IHIDEnumerator>(pEnum));

    REQUIRE(WaitFor([&]() { return iConnectedCount >= 2; }, 5000));

    SMXInfo info0, info1;
    SMX_GetInfo(0, &info0);
    SMX_GetInfo(1, &info1);
    CHECK(info0.m_bConnected);
    CHECK(info1.m_bConnected);
    CHECK_FALSE(info0.m_bIsPlayer2);
    CHECK(info1.m_bIsPlayer2);

    MESSAGE("Both pads: slot0 fw=", info0.m_iFirmwareVersion,
            " slot1 fw=", info1.m_iFirmwareVersion);

    SMX_Stop();
}

TEST_CASE("Replay: force recalibration command in capture")
{
    // Try p1_solo first, fall back to both_pads
    string sFile = CapturePath("p1_solo/device_0.smxhid");
    if(!CaptureExists(sFile))
        sFile = CapturePath("both_pads/device_0.smxhid");
    if(!CaptureExists(sFile))
    {
        MESSAGE("No capture found — skipping");
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

    // Send recalibration and verify it doesn't crash
    SMX_ForceRecalibration(0);
    this_thread::sleep_for(chrono::milliseconds(200));

    SMXInfo info;
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);

    // Verify the SDK sent the recalibration command ("C\n")
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetActualWrites(), string("C\n", 2)));
    MESSAGE("Force recalibration command verified in replay writes");

    SMX_Stop();
}

TEST_CASE("Replay: panel test mode command in capture")
{
    string sFile = CapturePath("p1_solo/device_0.smxhid");
    if(!CaptureExists(sFile))
        sFile = CapturePath("both_pads/device_0.smxhid");
    if(!CaptureExists(sFile))
    {
        MESSAGE("No capture found — skipping");
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

    // Enable and disable test mode
    SMX_SetPanelTestMode(PanelTestMode_PressureTest);
    this_thread::sleep_for(chrono::milliseconds(200));
    SMX_SetPanelTestMode(PanelTestMode_Off);
    this_thread::sleep_for(chrono::milliseconds(200));

    SMXInfo info;
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);

    // Verify the SDK sent panel test mode commands.
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetActualWrites(), string("t 1\n", 4)));
    CHECK(WritesContainCommand(devs[0]->GetActualWrites(), string("t 0\n", 4)));
    MESSAGE("Panel test mode commands verified in replay writes");

    SMX_Stop();
}

TEST_CASE("Replay: re-enable auto lights command in capture")
{
    string sFile = CapturePath("p1_solo/device_0.smxhid");
    if(!CaptureExists(sFile))
        sFile = CapturePath("both_pads/device_0.smxhid");
    if(!CaptureExists(sFile))
    {
        MESSAGE("No capture found — skipping");
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

    // Send re-enable auto lights
    SMX_ReenableAutoLights();
    this_thread::sleep_for(chrono::milliseconds(200));

    SMXInfo info;
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);

    // Verify the SDK sent the auto lights command ("S 1\n")
    auto &devs = pEnum->GetOpenedDevices();
    REQUIRE(devs.size() >= 1);
    CHECK(WritesContainCommand(devs[0]->GetActualWrites(), string("S 1\n", 4)));
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
