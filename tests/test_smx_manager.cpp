#include <doctest/doctest.h>
#include "SMX.h"
#include "SMXHIDInterface.h"

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

// --- FakeHIDDevice for manager tests ---

class ManagerFakeDevice : public IHIDDevice
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
        // Check if this is an activation command ("G" or "g\n")
        // HID packet format: [report_id=5][flags][size][payload...]
        if(len >= 4 && buf[0] == 5 && buf[2] >= 1)
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

// --- Helper to build device info response ---

static vector<uint8_t> MakeDeviceInfoResponse(char player, uint16_t fwVersion)
{
    // Report 6 with DEVICE_INFO flag (0x80)
    // Payload: data_info_packet = cmd(1) + packet_size(1) + player(1) + unused(1) + serial(16) + fw(2) + unused(1)
    vector<uint8_t> payload(23, 0);
    payload[0] = 'I';
    payload[1] = 23;
    payload[2] = static_cast<uint8_t>(player);
    // serial bytes: use distinct values so we can verify
    for(int i = 0; i < 16; i++)
        payload[4 + i] = static_cast<uint8_t>(player == '0' ? 0xA0 + i : 0xB0 + i);
    memcpy(&payload[20], &fwVersion, 2);

    vector<uint8_t> pkt;
    pkt.push_back(0x06);  // report ID
    pkt.push_back(0x80);  // DEVICE_INFO flag
    pkt.push_back(static_cast<uint8_t>(payload.size()));
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

// --- Helper to build a config response (needed for "fully connected") ---
// The device sends a 'G' config packet after activation.
// Format: START|END|HOST_CMD_FINISHED (0x07), payload starts with 'G' + size + config data

static vector<uint8_t> MakeConfigResponse()
{
    // Config payload: 'G' + size byte + config data
    // Must fit in 61 bytes (64 byte HID packet - 3 byte header)
    vector<uint8_t> payload;
    payload.push_back('G');
    payload.push_back(40);  // size of config data (small enough to fit)
    payload.resize(2 + 40, 0);  // zero-filled config

    vector<uint8_t> pkt;
    pkt.push_back(0x06);
    pkt.push_back(0x07);  // START|END|HOST_CMD_FINISHED
    pkt.push_back(static_cast<uint8_t>(payload.size()));
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

// --- FakeHIDEnumerator ---

class FakeHIDEnumerator : public IHIDEnumerator
{
public:
    void Init() override {}
    void Exit() override {}

    void AddDevice(const string &path, ManagerFakeDevice *pDevice)
    {
        m_aDevices.push_back({path, pDevice});
    }

    vector<HIDDeviceInfo> Enumerate(uint16_t, uint16_t) override
    {
        vector<HIDDeviceInfo> results;
        for(const auto &d : m_aDevices)
        {
            HIDDeviceInfo info;
            info.sPath = d.sPath;
            info.sProduct = L"StepManiaX";
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
                // Return a non-owning wrapper since we need to keep the pointer for queuing reads
                return unique_ptr<IHIDDevice>(new DeviceWrapper(d.pDevice));
            }
        }
        return nullptr;
    }

private:
    // Wrapper that delegates to the shared ManagerFakeDevice without owning it
    class DeviceWrapper : public IHIDDevice
    {
    public:
        explicit DeviceWrapper(ManagerFakeDevice *p) : m_p(p) {}
        int Read(uint8_t *buf, size_t len) override { return m_p->Read(buf, len); }
        int Write(const uint8_t *buf, size_t len) override { return m_p->Write(buf, len); }
        void Close() override {}
    private:
        ManagerFakeDevice *m_p;
    };

    struct DeviceEntry {
        string sPath;
        ManagerFakeDevice *pDevice;
        bool bOpened = false;
    };
    vector<DeviceEntry> m_aDevices;
};

// --- Helper: wait for a condition with timeout ---

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
// Device discovery and player ordering tests
// =========================================================================

TEST_CASE("Single P1 device is discovered and connected") {
    auto pFakeDevice = new ManagerFakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    // Queue device info response (P1)
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    // Config response will be queued when activation command is written
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMXInfo infoResult = {};
    int connectedPad = -1;
    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_Connected)
            *static_cast<int*>(pUser) = pad;
    };

    SMX_StartWithEnumerator(callback, &connectedPad, unique_ptr<IHIDEnumerator>(pEnum));

    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &infoResult);
        return infoResult.m_bConnected;
    });

    CHECK(bConnected);
    CHECK_FALSE(infoResult.m_bIsPlayer2);
    CHECK(infoResult.m_iFirmwareVersion == 5);
    CHECK(connectedPad == 0);

    SMX_Stop();
}

TEST_CASE("Single P2 device is placed in slot 1") {
    auto pFakeDevice = new ManagerFakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    // Queue device info response (P2: player='1')
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info1 = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(1, &info1);
        return info1.m_bConnected;
    });

    CHECK(bConnected);
    CHECK(info1.m_bIsPlayer2);

    // Slot 0 should be empty
    SMXInfo info0 = {};
    SMX_GetInfo(0, &info0);
    CHECK_FALSE(info0.m_bConnected);

    SMX_Stop();
}

TEST_CASE("Two devices are ordered P1=slot0, P2=slot1") {
    auto pFakeP1 = new ManagerFakeDevice();
    auto pFakeP2 = new ManagerFakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    // Add P2 first to test that ordering corrects it
    pEnum->AddDevice("/dev/hidraw0", pFakeP2);
    pEnum->AddDevice("/dev/hidraw1", pFakeP1);

    pFakeP2->QueueRead(MakeDeviceInfoResponse('1', 5));
    pFakeP2->SetConfigResponse(MakeConfigResponse());
    pFakeP1->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeP1->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBothConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });

    CHECK(bBothConnected);
    CHECK_FALSE(info0.m_bIsPlayer2);  // slot 0 = P1
    CHECK(info1.m_bIsPlayer2);         // slot 1 = P2

    SMX_Stop();
}
