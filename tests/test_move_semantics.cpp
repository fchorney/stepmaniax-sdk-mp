#include <doctest/doctest.h>
#include "test_helpers_manager.h"

#include <mutex>
#include <utility>
#include <vector>

using namespace std;
using namespace SMX;
using namespace SMXTestHelpers;

// =========================================================================
// Move semantics / pad swap regression tests
// =========================================================================

TEST_CASE("Pad swap: connected callback fires with correct pad index after reorder") {
    // P2 device is enumerated first (slot 0), P1 second (slot 1).
    // After both connect, CorrectDeviceOrder() swaps them.
    // The Connected callback must fire with pad=0 for P1 and pad=1 for P2.
    auto pFakeP2 = new FakeDevice();
    auto pFakeP1 = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeP2);
    pEnum->AddDevice("/dev/hidraw1", pFakeP1);

    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));  // P2
    pFakeP2->SetConfigResponse(MakeConfigResponse());
    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));  // P1
    pFakeP1->SetConfigResponse(MakeConfigResponse());

    // Track which pad indices received Connected callbacks
    struct CallbackData {
        mutex mtx;
        vector<int> connectedPads;
    } data;

    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_Connected)
        {
            auto *d = static_cast<CallbackData*>(pUser);
            lock_guard<mutex> lock(d->mtx);
            d->connectedPads.push_back(pad);
        }
    };

    SMX_StartWithEnumerator(callback, &data, unique_ptr<IHIDEnumerator>(pEnum));

    // Wait for both to connect
    SMXInfo info0 = {}, info1 = {};
    bool bBoth = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });
    REQUIRE(bBoth);

    // Verify ordering is correct
    CHECK_FALSE(info0.m_bIsPlayer2);
    CHECK(info1.m_bIsPlayer2);

    // Verify callbacks fired with correct pad indices (0 and 1 both present)
    lock_guard<mutex> lock(data.mtx);
    bool bGotPad0 = false, bGotPad1 = false;
    for(int p : data.connectedPads)
    {
        if(p == 0) bGotPad0 = true;
        if(p == 1) bGotPad1 = true;
    }
    CHECK(bGotPad0);
    CHECK(bGotPad1);

    SMX_Stop();
}

TEST_CASE("Pad swap: input state callback fires for correct pad after reorder") {
    // After a swap, the input state changed lambda (which captures 'this')
    // must be re-bound via SetConnectionCallbacks(). If not, input state
    // changes would fire with the wrong pad index.
    auto pFakeP2 = new FakeDevice();
    auto pFakeP1 = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeP2);
    pEnum->AddDevice("/dev/hidraw1", pFakeP1);

    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeP2->SetConfigResponse(MakeConfigResponse());
    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeP1->SetConfigResponse(MakeConfigResponse());

    // Track input state callbacks: record (pad, reason) pairs
    struct CallbackData {
        mutex mtx;
        vector<pair<int, int>> inputCallbacks;
    } data;

    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_InputState)
        {
            auto *d = static_cast<CallbackData*>(pUser);
            lock_guard<mutex> lock(d->mtx);
            d->inputCallbacks.emplace_back(pad, reason);
        }
    };

    SMX_StartWithEnumerator(callback, &data, unique_ptr<IHIDEnumerator>(pEnum));

    // Wait for both to connect (swap will have occurred)
    SMXInfo info0 = {}, info1 = {};
    bool bBoth = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });
    REQUIRE(bBoth);
    REQUIRE_FALSE(info0.m_bIsPlayer2);  // slot 0 = P1
    REQUIRE(info1.m_bIsPlayer2);         // slot 1 = P2

    // Clear any input callbacks from connection phase
    {
        lock_guard<mutex> lock(data.mtx);
        data.inputCallbacks.clear();
    }

    // Send input state to P1 device (which was swapped into slot 0)
    pFakeP1->QueueRead({HID_REPORT_INPUT_STATE, 0x0F, 0x00});  // state = 0x000F

    bool bGotP1Input = WaitFor([&]() {
        return SMX_GetInputState(0) == 0x000F;
    });
    CHECK(bGotP1Input);

    // Send input state to P2 device (which was swapped into slot 1)
    pFakeP2->QueueRead({HID_REPORT_INPUT_STATE, 0xF0, 0x00});  // state = 0x00F0

    bool bGotP2Input = WaitFor([&]() {
        return SMX_GetInputState(1) == 0x00F0;
    });
    CHECK(bGotP2Input);

    // Verify input callbacks fired with correct pad indices
    lock_guard<mutex> lock(data.mtx);
    bool bGotPad0Input = false, bGotPad1Input = false;
    for(const auto &cb : data.inputCallbacks)
    {
        if(cb.first == 0) bGotPad0Input = true;
        if(cb.first == 1) bGotPad1Input = true;
    }
    CHECK(bGotPad0Input);
    CHECK(bGotPad1Input);

    SMX_Stop();
}

TEST_CASE("Pad swap: config state survives reorder") {
    // After a swap, GetConfig should still return the correct config for each pad.
    SMXConfig p1Config = {};
    p1Config.panelDebounceMicroseconds = 1111;

    SMXConfig p2Config = {};
    p2Config.panelDebounceMicroseconds = 2222;

    auto pFakeP2 = new FakeDevice();
    auto pFakeP1 = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    // P2 enumerated first to force a swap
    pEnum->AddDevice("/dev/hidraw0", pFakeP2);
    pEnum->AddDevice("/dev/hidraw1", pFakeP1);

    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeP2->SetConfigResponsePackets(MakeFullConfigResponsePackets(p2Config));
    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeP1->SetConfigResponsePackets(MakeFullConfigResponsePackets(p1Config));

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBoth = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });
    REQUIRE(bBoth);
    REQUIRE_FALSE(info0.m_bIsPlayer2);
    REQUIRE(info1.m_bIsPlayer2);

    // After swap, slot 0 (P1) should have p1Config, slot 1 (P2) should have p2Config
    SMXConfig cfg0 = {}, cfg1 = {};
    REQUIRE(SMX_GetConfig(0, &cfg0));
    REQUIRE(SMX_GetConfig(1, &cfg1));
    CHECK(cfg0.panelDebounceMicroseconds == 1111);
    CHECK(cfg1.panelDebounceMicroseconds == 2222);

    SMX_Stop();
}
