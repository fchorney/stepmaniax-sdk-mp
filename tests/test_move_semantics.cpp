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

TEST_CASE("Pad swap: input state is readable from correct slot after reorder") {
    // Verify that after a swap, SMX_GetInputState(0) reads from the P1 device
    // and SMX_GetInputState(1) reads from the P2 device.
    auto pFakeP2 = new FakeDevice();
    auto pFakeP1 = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    // P2 enumerated first → will be swapped to slot 1
    pEnum->AddDevice("/dev/hidraw0", pFakeP2);
    pEnum->AddDevice("/dev/hidraw1", pFakeP1);

    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeP2->SetConfigResponse(MakeConfigResponse());
    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeP1->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBoth = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });
    REQUIRE(bBoth);

    // Send distinct input states to each physical device
    pFakeP1->QueueRead({HID_REPORT_INPUT_STATE, 0xAA, 0x00});  // P1 → slot 0
    pFakeP2->QueueRead({HID_REPORT_INPUT_STATE, 0x55, 0x00});  // P2 → slot 1

    bool bP1State = WaitFor([&]() {
        return SMX_GetInputState(0) == 0x00AA;
    });
    CHECK(bP1State);

    bool bP2State = WaitFor([&]() {
        return SMX_GetInputState(1) == 0x0055;
    });
    CHECK(bP2State);

    // Cross-check: states are NOT swapped
    CHECK(SMX_GetInputState(0) != 0x0055);
    CHECK(SMX_GetInputState(1) != 0x00AA);

    SMX_Stop();
}

TEST_CASE("Pad swap: update callback uses correct pad index (not stale 'this')") {
    // This specifically tests that CallUpdateCallback() inside SMXDevice uses
    // the device's own connection info (which has the correct P2 flag) after
    // the move. If the lambda captured a stale 'this', it would read garbage
    // or the wrong device's info.
    auto pFakeP2 = new FakeDevice();
    auto pFakeP1 = new FakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeP2);
    pEnum->AddDevice("/dev/hidraw1", pFakeP1);

    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeP2->SetConfigResponse(MakeConfigResponse());
    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeP1->SetConfigResponse(MakeConfigResponse());

    // Record ALL callbacks with their pad index and reason
    struct CallbackData {
        mutex mtx;
        vector<pair<int, int>> allCallbacks;
    } data;

    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        auto *d = static_cast<CallbackData*>(pUser);
        lock_guard<mutex> lock(d->mtx);
        d->allCallbacks.emplace_back(pad, reason);
    };

    SMX_StartWithEnumerator(callback, &data, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBoth = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });
    REQUIRE(bBoth);

    // After swap + connection, verify no callback ever fired with an invalid pad index
    lock_guard<mutex> lock(data.mtx);
    for(const auto &cb : data.allCallbacks)
    {
        CHECK((cb.first == 0 || cb.first == 1));
    }

    // Verify we got callbacks for both pads
    bool bSawPad0 = false, bSawPad1 = false;
    for(const auto &cb : data.allCallbacks)
    {
        if(cb.first == 0) bSawPad0 = true;
        if(cb.first == 1) bSawPad1 = true;
    }
    CHECK(bSawPad0);
    CHECK(bSawPad1);

    SMX_Stop();
}
