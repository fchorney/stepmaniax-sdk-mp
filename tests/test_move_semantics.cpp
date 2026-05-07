#include <doctest/doctest.h>
#include "SMX.h"
#include "SMXDeviceConnection.h"
#include "SMXHIDInterface.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace std;
using namespace SMX;

// Declared in SMX.cpp (test-only, not exported from shared lib)
extern void SMX_StartWithEnumerator(SMXUpdateCallback callback, void *pUser,
                                     std::unique_ptr<SMX::IHIDEnumerator> pEnumerator);

namespace {

// --- FakeHIDDevice ---

class FakeDevice : public IHIDDevice
{
public:
    void QueueRead(vector<uint8_t> packet)
    {
        lock_guard<mutex> lock(m_Mutex);
        m_aReads.push(std::move(packet));
    }

    int Read(uint8_t *buf, size_t len) override
    {
        lock_guard<mutex> lock(m_Mutex);
        if(m_aReads.empty())
            return 0;
        auto &pkt = m_aReads.front();
        size_t n = min(len, pkt.size());
        memcpy(buf, pkt.data(), n);
        m_aReads.pop();
        return static_cast<int>(n);
    }

    int Write(const uint8_t *buf, size_t len) override
    {
        // Auto-respond to activation command with config
        if(len >= 4 && buf[0] == HID_REPORT_COMMAND && buf[2] >= 1)
        {
            char cmd = static_cast<char>(buf[3]);
            if(cmd == 'G' || cmd == 'g')
            {
                lock_guard<mutex> lock(m_Mutex);
                if(!m_ConfigResponse.empty())
                    m_aReads.push(m_ConfigResponse);
            }
        }
        return static_cast<int>(len);
    }

    void Close() override {}

    void SetConfigResponse(vector<uint8_t> resp)
    {
        m_ConfigResponse = std::move(resp);
    }

private:
    mutex m_Mutex;
    queue<vector<uint8_t>> m_aReads;
    vector<uint8_t> m_ConfigResponse;
};

// --- Helpers ---

static vector<uint8_t> MakeDeviceInfoResponse(char player, uint16_t fwVersion)
{
    vector<uint8_t> payload(23, 0);
    payload[0] = 'I';
    payload[1] = 23;
    payload[2] = static_cast<uint8_t>(player);
    for(int i = 0; i < 16; i++)
        payload[4 + i] = static_cast<uint8_t>(player == '0' ? 0xA0 + i : 0xB0 + i);
    memcpy(&payload[20], &fwVersion, 2);

    vector<uint8_t> pkt;
    pkt.push_back(HID_REPORT_DATA);
    pkt.push_back(PACKET_FLAG_DEVICE_INFO);
    pkt.push_back(static_cast<uint8_t>(payload.size()));
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

static vector<uint8_t> MakeConfigResponse()
{
    vector<uint8_t> payload;
    payload.push_back('G');
    payload.push_back(40);
    payload.resize(2 + 40, 0);

    vector<uint8_t> pkt;
    pkt.push_back(HID_REPORT_DATA);
    pkt.push_back(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED);
    pkt.push_back(static_cast<uint8_t>(payload.size()));
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

class FakeHIDEnumerator : public IHIDEnumerator
{
public:
    void Init() override {}
    void Exit() override {}

    void AddDevice(const string &path, FakeDevice *pDevice)
    {
        m_aDevices.push_back({path, pDevice, false});
    }

    vector<HIDDeviceInfo> Enumerate(uint16_t, uint16_t) override
    {
        vector<HIDDeviceInfo> results;
        for(const auto &d : m_aDevices)
        {
            HIDDeviceInfo info;
            info.sPath = d.sPath;
            info.sProduct = SMX_USB_PRODUCT_STRING;
            results.push_back(info);
        }
        return results;
    }

    unique_ptr<IHIDDevice> Open(const string &path) override
    {
        for(auto &d : m_aDevices)
        {
            if(d.sPath == path && !d.bOpened)
            {
                d.bOpened = true;
                return unique_ptr<IHIDDevice>(new DeviceWrapper(d.pDevice));
            }
        }
        return nullptr;
    }

private:
    class DeviceWrapper : public IHIDDevice
    {
    public:
        explicit DeviceWrapper(FakeDevice *p) : m_p(p) {}
        int Read(uint8_t *buf, size_t len) override { return m_p->Read(buf, len); }
        int Write(const uint8_t *buf, size_t len) override { return m_p->Write(buf, len); }
        void Close() override {}
    private:
        FakeDevice *m_p;
    };

    struct DeviceEntry {
        string sPath;
        FakeDevice *pDevice;
        bool bOpened;
    };
    vector<DeviceEntry> m_aDevices;
};

static bool WaitFor(function<bool()> cond, int timeoutMs = 2000)
{
    auto deadline = chrono::steady_clock::now() + chrono::milliseconds(timeoutMs);
    while(!cond())
    {
        if(chrono::steady_clock::now() > deadline)
            return false;
        this_thread::sleep_for(chrono::milliseconds(10));
    }
    return true;
}

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

} // anonymous namespace
