#include <doctest/doctest.h>
#include "SMX.h"
#include "SMXDeviceConnection.h"
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
        if(m_iFailAfterReads > 0)
        {
            m_iReadCount++;
            if(m_iReadCount > m_iFailAfterReads)
                return -1;
        }
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
        {
            lock_guard<mutex> lock(m_Mutex);
            if(m_bFailWrites)
                return -1;
        }
        if(m_bCaptureWrites)
        {
            lock_guard<mutex> lock(m_Mutex);
            m_aCapturedWrites.emplace_back(buf, buf + len);
        }
        // Check if this is an activation command ("G" or "g\n")
        // HID packet format: [report_id=5][flags][size][payload...]
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

    void SetFailReadsAfterCount(int count)
    {
        lock_guard<mutex> lock(m_Mutex);
        m_iFailAfterReads = count;
        m_iReadCount = 0;
    }

    void SetFailWrites(bool b)
    {
        lock_guard<mutex> lock(m_Mutex);
        m_bFailWrites = b;
    }

    void SetCaptureWrites(bool b) { m_bCaptureWrites = b; }

    void ClearCapturedWrites()
    {
        lock_guard<mutex> lock(m_Mutex);
        m_aCapturedWrites.clear();
    }

    int GetCapturedWriteCount()
    {
        lock_guard<mutex> lock(m_Mutex);
        return static_cast<int>(m_aCapturedWrites.size());
    }

    vector<vector<uint8_t>> GetCapturedWrites()
    {
        lock_guard<mutex> lock(m_Mutex);
        return m_aCapturedWrites;
    }

private:
    mutex m_Mutex;
    queue<vector<uint8_t>> m_aReads;
    vector<uint8_t> m_ConfigResponse;
    int m_iFailAfterReads = 0;
    int m_iReadCount = 0;
    bool m_bFailWrites = false;
    bool m_bCaptureWrites = false;
    vector<vector<uint8_t>> m_aCapturedWrites;
};

// --- Helper to build device info response ---

static vector<uint8_t> MakeDeviceInfoResponse(char player, uint16_t fwVersion)
{
    // Report 6 with DEVICE_INFO flag
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
    pkt.push_back(HID_REPORT_DATA);  // report ID
    pkt.push_back(PACKET_FLAG_DEVICE_INFO);
    pkt.push_back(static_cast<uint8_t>(payload.size()));
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

// --- Helper to build a config response (needed for "fully connected") ---
// The device sends a 'G' config packet after activation.
// Format: START|END|HOST_CMD_FINISHED, payload starts with 'G' + size + config data

static vector<uint8_t> MakeConfigResponse()
{
    // Config payload: 'G' + size byte + config data
    // Must fit in 61 bytes (64 byte HID packet - 3 byte header)
    vector<uint8_t> payload;
    payload.push_back('G');
    payload.push_back(40);  // size of config data (small enough to fit)
    payload.resize(2 + 40, 0);  // zero-filled config

    vector<uint8_t> pkt;
    pkt.push_back(HID_REPORT_DATA);
    pkt.push_back(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED);
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

// =========================================================================
// Disconnect and reconnect
// =========================================================================

TEST_CASE("Device disconnect fires callback and clears slot") {
    auto pFakeDevice = new ManagerFakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    int iDisconnectedPad = -1;
    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_Disconnected)
            *static_cast<int*>(pUser) = pad;
    };

    SMX_StartWithEnumerator(callback, &iDisconnectedPad, unique_ptr<IHIDEnumerator>(pEnum));

    // Wait for connection
    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    // Trigger disconnect by making writes fail. The main thread will detect
    // the write error in CheckWrites and close the device.
    pFakeDevice->SetFailWrites(true);

    // Send a command to trigger a write attempt
    SMX_SetSerialNumbers();

    // Wait for disconnect
    bool bDisconnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return !info.m_bConnected;
    });

    CHECK(bDisconnected);
    CHECK(iDisconnectedPad == 0);

    SMX_Stop();
}

// =========================================================================
// Duplicate player jumpers
// =========================================================================

TEST_CASE("Duplicate player jumpers: both P1 are assigned to slots without swap") {
    auto pFakeA = new ManagerFakeDevice();
    auto pFakeB = new ManagerFakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeA);
    pEnum->AddDevice("/dev/hidraw1", pFakeB);

    // Both devices report as P1 (player='0')
    pFakeA->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeA->SetConfigResponse(MakeConfigResponse());
    pFakeB->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeB->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info0 = {}, info1 = {};
    bool bBothConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info0);
        SMX_GetInfo(1, &info1);
        return info0.m_bConnected && info1.m_bConnected;
    });

    CHECK(bBothConnected);
    // Both should report as P1 (same jumper)
    CHECK_FALSE(info0.m_bIsPlayer2);
    CHECK_FALSE(info1.m_bIsPlayer2);

    SMX_Stop();
}

// =========================================================================
// SMX_GetInputState through full stack
// =========================================================================

TEST_CASE("SMX_GetInputState returns panel state through full stack") {
    auto pFakeDevice = new ManagerFakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    // Wait for connection
    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    // Queue a Report 3 input state packet
    pFakeDevice->QueueRead({HID_REPORT_INPUT_STATE, 0x55, 0x01});  // state = 0x0155

    bool bGotState = WaitFor([&]() {
        return SMX_GetInputState(0) == 0x0155;
    });

    CHECK(bGotState);

    // Pad 1 should be 0 (not connected)
    CHECK(SMX_GetInputState(1) == 0);

    SMX_Stop();
}

// =========================================================================
// SMX_SetSerialNumbers command format
// =========================================================================

TEST_CASE("SMX_SetSerialNumbers sends 's' command with 16-byte serial") {
    auto pFakeDevice = new ManagerFakeDevice();
    pFakeDevice->SetCaptureWrites(true);
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });
    REQUIRE(bConnected);

    pFakeDevice->ClearCapturedWrites();
    SMX_SetSerialNumbers();

    // Wait for the command to be written
    bool bGotWrite = WaitFor([&]() {
        return pFakeDevice->GetCapturedWriteCount() > 0;
    });
    REQUIRE(bGotWrite);

    // Verify the command format: report_id=5, flags, size, payload starts with 's'
    auto writes = pFakeDevice->GetCapturedWrites();
    bool bFoundSerialCmd = false;
    for(const auto &w : writes)
    {
        if(w.size() >= 4 && w[0] == HID_REPORT_COMMAND)
        {
            uint8_t payloadSize = w[2];
            if(payloadSize >= 18 && w[3] == 's')  // 's' + 16 bytes serial + '\n'
            {
                bFoundSerialCmd = true;
                CHECK(payloadSize == 18);  // 's' + 16 + '\n'
                CHECK(w[3 + 17] == '\n');
                break;
            }
        }
    }
    CHECK(bFoundSerialCmd);

    SMX_Stop();
}

// =========================================================================
// SMX_GetInfo edge cases
// =========================================================================

TEST_CASE("SMX_GetInfo on disconnected pad returns not connected") {
    auto pEnum = new FakeHIDEnumerator();
    // No devices added

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    // Give it a moment to start
    this_thread::sleep_for(chrono::milliseconds(50));

    SMXInfo info0 = {}, info1 = {};
    SMX_GetInfo(0, &info0);
    SMX_GetInfo(1, &info1);

    CHECK_FALSE(info0.m_bConnected);
    CHECK_FALSE(info1.m_bConnected);
    CHECK(info0.m_iFirmwareVersion == 0);

    SMX_Stop();
}

TEST_CASE("SMX_GetInfo with invalid pad index does not crash") {
    auto pEnum = new FakeHIDEnumerator();
    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    this_thread::sleep_for(chrono::milliseconds(50));

    SMXInfo info = {};
    info.m_bConnected = true;  // set to true to verify it's NOT modified
    SMX_GetInfo(-1, &info);
    CHECK(info.m_bConnected);  // unchanged since GetDevice returns nullptr

    SMX_GetInfo(2, &info);
    CHECK(info.m_bConnected);  // unchanged

    SMX_Stop();
}

// =========================================================================
// Config packet parsing (old/new format, invalid)
// =========================================================================

TEST_CASE("Device with firmware >= 5 uses 'G' (new config format)") {
    auto pFakeDevice = new ManagerFakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 5));
    pFakeDevice->SetConfigResponse(MakeConfigResponse());

    int iConfigUpdated = 0;
    auto callback = [](int pad, SMXUpdateCallbackReason reason, void *pUser) {
        if(reason & SMXUpdateCallback_ConfigUpdated)
            (*static_cast<int*>(pUser))++;
    };

    SMX_StartWithEnumerator(callback, &iConfigUpdated, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });

    CHECK(bConnected);
    CHECK(iConfigUpdated > 0);

    SMX_Stop();
}

TEST_CASE("Device with firmware < 5 uses 'g' (old config format) and converts") {
    auto pFakeDevice = new ManagerFakeDevice();
    auto pEnum = new FakeHIDEnumerator();
    pEnum->AddDevice("/dev/hidraw0", pFakeDevice);

    // Firmware version 4 (< 5) → uses 'g' command
    pFakeDevice->QueueRead(MakeDeviceInfoResponse('0', 4));
    // Build old-format config response: 'g' + size + old config data
    vector<uint8_t> oldConfigPayload;
    oldConfigPayload.push_back('g');
    oldConfigPayload.push_back(40);
    oldConfigPayload.resize(2 + 40, 0);

    vector<uint8_t> oldConfigPkt;
    oldConfigPkt.push_back(HID_REPORT_DATA);
    oldConfigPkt.push_back(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED);
    oldConfigPkt.push_back(static_cast<uint8_t>(oldConfigPayload.size()));
    oldConfigPkt.insert(oldConfigPkt.end(), oldConfigPayload.begin(), oldConfigPayload.end());
    pFakeDevice->SetConfigResponse(oldConfigPkt);

    SMX_StartWithEnumerator([](int, SMXUpdateCallbackReason, void*){},
                            nullptr, unique_ptr<IHIDEnumerator>(pEnum));

    SMXInfo info = {};
    bool bConnected = WaitFor([&]() {
        SMX_GetInfo(0, &info);
        return info.m_bConnected;
    });

    CHECK(bConnected);
    CHECK(info.m_iFirmwareVersion == 4);

    SMX_Stop();
}
