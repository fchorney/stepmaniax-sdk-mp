#include <doctest/doctest.h>
#include "SMXDeviceConnection.h"
#include "SMXHIDInterface.h"

#include <cstring>
#include <queue>
#include <vector>

using namespace std;
using namespace SMX;

// --- FakeHIDDevice: injectable test double for IHIDDevice ---

class FakeHIDDevice : public IHIDDevice
{
public:
    void QueueRead(vector<uint8_t> packet) { m_aReads.push(std::move(packet)); }

    int Read(uint8_t *buf, size_t len) override
    {
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
        m_aWrites.emplace_back(buf, buf + len);
        return static_cast<int>(len);
    }

    void Close() override { m_bClosed = true; }

    const vector<vector<uint8_t>> &GetWrites() const { return m_aWrites; }
    bool IsClosed() const { return m_bClosed; }

private:
    queue<vector<uint8_t>> m_aReads;
    vector<vector<uint8_t>> m_aWrites;
    bool m_bClosed = false;
};

// --- Tests ---

TEST_CASE("Report 3 updates input state") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    // Report 3: [report_id=3][low=0x10][high=0x00] — panel 4 pressed
    pFake->QueueRead({0x03, 0x10, 0x00});

    string sError;
    conn.PollUSBData(sError);

    CHECK(sError.empty());
    CHECK(conn.GetInputState() == 0x0010);
}

TEST_CASE("Report 3 updates with multiple panels") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    // Panels 0, 4, 8 pressed: bits 0, 4, 8 = 0x0111
    pFake->QueueRead({0x03, 0x11, 0x01});

    string sError;
    conn.PollUSBData(sError);

    CHECK(conn.GetInputState() == 0x0111);
}

TEST_CASE("Report 3 fires input state callback on change") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    int iCallbackCount = 0;
    conn.SetInputStateChangedCallback([&]() { iCallbackCount++; });

    pFake->QueueRead({0x03, 0x01, 0x00});

    string sError;
    conn.PollUSBData(sError);

    CHECK(iCallbackCount == 1);

    // Same state again — should NOT fire
    pFake->QueueRead({0x03, 0x01, 0x00});
    conn.PollUSBData(sError);

    CHECK(iCallbackCount == 1);
}

TEST_CASE("Report 3 always-fire mode fires on duplicate state") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));
    conn.SetAlwaysFireInputCallback(true);

    int iCallbackCount = 0;
    conn.SetInputStateChangedCallback([&]() { iCallbackCount++; });

    pFake->QueueRead({0x03, 0x01, 0x00});
    pFake->QueueRead({0x03, 0x01, 0x00});

    string sError;
    conn.PollUSBData(sError);

    CHECK(iCallbackCount == 2);
}

TEST_CASE("Report 6 is buffered for main thread") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    // Report 6: [id=6][flags=START|END=0x05][size=2][payload='Hi']
    pFake->QueueRead({0x06, 0x05, 0x02, 'H', 'i'});

    string sError;
    bool bHasReport6 = conn.PollUSBData(sError);

    CHECK(sError.empty());
    CHECK(bHasReport6);
}

TEST_CASE("PollUSBData returns false when no data") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    string sError;
    bool bHasData = conn.PollUSBData(sError);

    CHECK(sError.empty());
    CHECK_FALSE(bHasData);
}

TEST_CASE("Read error propagates") {
    // Custom fake that returns -1 on read
    class ErrorDevice : public IHIDDevice {
    public:
        int Read(uint8_t *, size_t) override { return -1; }
        int Write(const uint8_t *, size_t) override { return 0; }
        void Close() override {}
    };

    unique_ptr<IHIDDevice> pDevice(new ErrorDevice());

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    string sError;
    conn.PollUSBData(sError);

    CHECK_FALSE(sError.empty());
}

TEST_CASE("Close resets state") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    pFake->QueueRead({0x03, 0xFF, 0x00});
    string sError;
    conn.PollUSBData(sError);
    CHECK(conn.GetInputState() == 0x00FF);

    conn.Close();

    CHECK_FALSE(conn.IsConnected());
    CHECK(conn.GetInputState() == 0);
}
