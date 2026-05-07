#pragma once

// Shared test infrastructure for manager-level tests (tests that go through
// the full SMXManager stack with threading). Provides a thread-safe fake HID
// device, a fake enumerator, and packet-building helpers.

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

// Declared in SMX.cpp (test-only, not exported from shared lib)
extern void SMX_StartWithEnumerator(SMXUpdateCallback callback, void *pUser,
                                     std::unique_ptr<SMX::IHIDEnumerator> pEnumerator);

namespace SMXTestHelpers {

using namespace std;
using namespace SMX;

// --- Thread-safe fake HID device for manager tests ---

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
        // Auto-respond to activation command ("G" or "g\n") with config
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

// --- Fake HID enumerator ---

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
    // Non-owning wrapper that delegates to the shared FakeDevice
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

// --- Packet-building helpers ---

/// Builds a device info response packet (Report 6 with DEVICE_INFO flag).
/// @param player '0' for P1, '1' for P2
/// @param fwVersion Firmware version number
inline vector<uint8_t> MakeDeviceInfoResponse(char player, uint16_t fwVersion)
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

/// Builds a config response packet ('G' format, new firmware).
/// Queued as the auto-response when the device receives an activation command.
inline vector<uint8_t> MakeConfigResponse()
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

// --- Test utility ---

/// Polls a condition until it becomes true or timeout expires.
/// @param cond Condition to check
/// @param timeoutMs Maximum wait time in milliseconds (default 2000)
/// @return true if condition was met, false on timeout
inline bool WaitFor(function<bool()> cond, int timeoutMs = 2000)
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

} // namespace SMXTestHelpers
