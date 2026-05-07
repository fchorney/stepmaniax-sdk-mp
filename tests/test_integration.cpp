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
// To record HID traffic for regression tests:
//   SMX_CAPTURE_DIR=/tmp/captures ./smx-integration-tests

// --- Helper: get capture directory from environment ---

static string GetCaptureDir()
{
    const char *pDir = getenv("SMX_CAPTURE_DIR");
    return pDir ? string(pDir) : string();
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

TEST_CASE("Real hardware: device discovery and connection")
{
    // Check if hardware is present by enumerating
    auto pEnum = CreateHIDAPIEnumerator();
    pEnum->Init();
    auto devices = pEnum->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    pEnum->Exit();

    if(devices.empty())
    {
        MESSAGE("No SMX hardware detected, skipping integration test");
        return;
    }

    MESSAGE("Found ", devices.size(), " SMX device(s)");

    // Optionally wrap with recording enumerator
    string sCaptureDir = GetCaptureDir();
    unique_ptr<IHIDEnumerator> pEnumerator;
    if(!sCaptureDir.empty())
    {
        MESSAGE("Recording HID traffic to: ", sCaptureDir);
        pEnumerator.reset(new RecordingHIDEnumerator(CreateHIDAPIEnumerator(), sCaptureDir));
    }
    else
    {
        pEnumerator = CreateHIDAPIEnumerator();
    }

    // Start SDK with the enumerator
    bool bConnected = false;
    bool bGotInput = false;

    SMX_StartWithEnumerator(
        [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected, std::move(pEnumerator));

    // Wait for connection
    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    SMXInfo info;
    SMX_GetInfo(0, &info);
    CHECK(info.m_bConnected);
    CHECK(info.m_iFirmwareVersion > 0);
    MESSAGE("Connected: fw=", info.m_iFirmwareVersion,
            " serial=", info.m_bHasSerialNumber ? info.m_Serial : "(none)");

    // Read input state (just verify it doesn't crash)
    uint16_t iState = SMX_GetInputState(0);
    MESSAGE("Input state: 0x", std::hex, iState);

    // Let it run briefly to capture some traffic
    this_thread::sleep_for(chrono::milliseconds(500));

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

    bool bConnected = false;
    int iInputCount = 0;

    SMX_SetInputStateMode(true);  // fire on every packet
    SMX_Start(
        [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                *static_cast<bool *>(pUser) = true;
        },
        &bConnected);

    REQUIRE(WaitFor([&]() { return bConnected; }, 5000));

    // Collect input state for 1 second
    auto start = chrono::steady_clock::now();
    while(chrono::steady_clock::now() - start < chrono::seconds(1))
    {
        SMX_GetInputState(0);
        iInputCount++;
        this_thread::sleep_for(chrono::milliseconds(1));
    }

    MESSAGE("Polled input state ", iInputCount, " times in 1 second");
    CHECK(iInputCount > 0);

    SMX_Stop();
}
