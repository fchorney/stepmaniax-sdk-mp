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
    atomic<int> iConnectedCallbacks{0};

    SMX_StartWithEnumerator(
        [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
            if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
                static_cast<atomic<int>*>(pUser)->fetch_add(1);
        },
        &iConnectedCallbacks, std::move(pEnumerator));

    // Wait for all detected devices to connect
    int iExpected = static_cast<int>(devices.size());
    REQUIRE(WaitFor([&]() { return iConnectedCallbacks.load() >= iExpected; }, 5000));

    // Check all connected pads
    // Brief delay to allow USB polling thread to receive fresh input state
    // after device ordering is finalized.
    this_thread::sleep_for(chrono::milliseconds(200));

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

        uint16_t iState = SMX_GetInputState(i);
        char sHex[8];
        snprintf(sHex, sizeof(sHex), "%04x", iState);
        MESSAGE("Slot ", i, " input state: 0x", sHex);
    }
    CHECK(iConnectedCount == static_cast<int>(devices.size()));

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
    // Observed behavior: the device sends ~10 Report 3 packets/sec at idle (likely a
    // heartbeat) and ~50/sec during active panel input (one per state transition).
    // The rate is determined by the device firmware, not the SDK polling interval.
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
