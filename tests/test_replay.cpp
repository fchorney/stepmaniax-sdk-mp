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
        // Extract index from path "/replay/N"
        size_t iIdx = static_cast<size_t>(stoi(path.substr(8)));
        if(iIdx >= m_aCaptures.size())
            return nullptr;
        return unique_ptr<IHIDDevice>(new ReplayHIDDevice(m_aCaptures[iIdx]));
    }

private:
    vector<string> m_aCaptures;
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

// --- Replay regression tests ---

TEST_CASE("Replay: P1 solo connection")
{
    string sFile = CapturePath("p1_solo/device_0.smxhid");
    if(!CaptureExists(sFile))
    {
        MESSAGE("Capture not found: ", sFile, " — skipping");
        return;
    }

    auto pEnum = unique_ptr<ReplayHIDEnumerator>(new ReplayHIDEnumerator());
    pEnum->AddCapture(sFile);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, std::move(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    // P1 pad should be in slot 0
    SMXInfo info;
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);
    CHECK_FALSE(info.m_bIsPlayer2);
    CHECK(info.m_iFirmwareVersion > 0);
    MESSAGE("P1 solo: fw=", info.m_iFirmwareVersion,
            " serial=", info.m_bHasSerialNumber ? info.m_Serial : "(none)");

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

    auto pEnum = unique_ptr<ReplayHIDEnumerator>(new ReplayHIDEnumerator());
    pEnum->AddCapture(sFile);

    bool bConnected = false;
    SMX_StartWithEnumerator(
        [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, std::move(pEnum));

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    // P2 pad should be in slot 1
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

    auto pEnum = unique_ptr<ReplayHIDEnumerator>(new ReplayHIDEnumerator());
    pEnum->AddCapture(sFile0);
    pEnum->AddCapture(sFile1);

    int iConnectedCount = 0;
    SMX_StartWithEnumerator(
        [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                (*static_cast<int *>(pUser))++;
        },
        &iConnectedCount, std::move(pEnum));

    REQUIRE(WaitFor([&]() { return iConnectedCount >= 2; }, 5000));

    // Both slots should be connected
    SMXInfo info0, info1;
    SMX_GetInfo(0, &info0);
    SMX_GetInfo(1, &info1);
    CHECK(info0.m_bConnected);
    CHECK(info1.m_bConnected);
    CHECK(info0.m_iFirmwareVersion > 0);
    CHECK(info1.m_iFirmwareVersion > 0);

    // Slot 0 should be P1, slot 1 should be P2
    CHECK_FALSE(info0.m_bIsPlayer2);
    CHECK(info1.m_bIsPlayer2);

    MESSAGE("Both pads: slot0 fw=", info0.m_iFirmwareVersion,
            " slot1 fw=", info1.m_iFirmwareVersion);

    SMX_Stop();
}
