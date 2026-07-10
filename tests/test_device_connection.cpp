#include <doctest/doctest.h>
#include "SMX.h"
#include "SMXDeviceConnection.h"
#include "SMXHIDInterface.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <queue>
#include <thread>
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

    // These connection unit tests drive the poll handle a single call at a time
    // and assert on the result, so the blocking-first read just returns whatever
    // is queued (or nothing) immediately, like Read.
    int ReadTimeout(uint8_t *buf, size_t len, int /*iTimeoutMs*/) override
    {
        return Read(buf, len);
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

// Non-owning view of a FakeHIDDevice. The connection now opens two handles (a
// read handle for PollUSBData, a write handle for CheckWrites); in tests both
// delegate to one shared FakeHIDDevice, mirroring two real handles to a single
// physical device, so QueueRead feeds reads and GetWrites captures writes
// regardless of which handle performs the I/O.
class FakeHIDView : public IHIDDevice
{
public:
    explicit FakeHIDView(IHIDDevice *p) : m_p(p) {}
    int Read(uint8_t *buf, size_t len) override { return m_p->Read(buf, len); }
    int ReadTimeout(uint8_t *buf, size_t len, int iTimeoutMs) override { return m_p->ReadTimeout(buf, len, iTimeoutMs); }
    int Write(const uint8_t *buf, size_t len) override { return m_p->Write(buf, len); }
    void Close() override { m_p->Close(); }
private:
    IHIDDevice *m_p;
};

// Opens conn over a shared fake: the read handle is a non-owning view, the write
// handle owns pFake. Returns the read-side poll handle (run PollUSBData on it),
// with a no-op input callback. Replaces the old single-handle conn.Open.
static std::unique_ptr<SMXPollHandle> OpenSplit(SMXDeviceConnection &conn, IHIDDevice *pFake, const std::string &path = "/fake/path")
{
    return conn.Open(path, unique_ptr<IHIDDevice>(new FakeHIDView(pFake)),
                    unique_ptr<IHIDDevice>(pFake), nullptr, 0);
}

// --- Helper: build a Report 6 packet for PollUSBData ---
// Format: [report_id][flags][payload_size][payload...]
static vector<uint8_t> MakeReport6(uint8_t flags, const vector<uint8_t> &payload)
{
    vector<uint8_t> pkt;
    pkt.push_back(HID_REPORT_DATA);
    pkt.push_back(flags);
    pkt.push_back(static_cast<uint8_t>(payload.size()));
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    return pkt;
}

// --- Helper: build a device info response packet ---
// data_info_packet: cmd(1), packet_size(1), player(1), unused(1), serial(16), fw_version(2), unused(1)
static vector<uint8_t> MakeDeviceInfoPayload(char player, uint16_t fwVersion, const uint8_t serial[SERIAL_SIZE])
{
    vector<uint8_t> payload(23, 0);
    payload[0] = 'I';  // cmd
    payload[1] = 23;   // packet_size
    payload[2] = static_cast<uint8_t>(player);
    payload[3] = 0;    // unused
    memcpy(&payload[4], serial, SERIAL_SIZE);
    memcpy(&payload[20], &fwVersion, 2);
    payload[22] = 0;   // unused
    return payload;
}

// --- Helper: complete the device info handshake ---
// Open() queues a RequestDeviceInfo command. Update() sends it via CheckWrites.
// Then we feed the device info response and call Update() again to process it.
static void CompleteDeviceInfoHandshake(SMXDeviceConnection &conn, SMXPollHandle *poll, FakeHIDDevice *pFake,
                                         char player = '0', uint16_t fwVersion = 5)
{
    string sError;
    // First Update sends the queued device info request
    conn.Update(sError);

    // Build and feed device info response
    uint8_t serial[SERIAL_SIZE] = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,
                          0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f,0x10};
    auto payload = MakeDeviceInfoPayload(player, fwVersion, serial);
    pFake->QueueRead(MakeReport6(PACKET_FLAG_DEVICE_INFO, payload));  // DEVICE_INFO flag
    poll->PollUSBData(0);

    // Second Update processes the device info response
    conn.Update(sError);
}

// =========================================================================
// Report 3 tests (existing)
// =========================================================================

TEST_CASE("Report 3 updates input state") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pFake)), std::move(pDevice), nullptr, 0);

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x10, 0x00});

    poll->PollUSBData(0);

    CHECK(conn.GetInputState() == 0x0010);
}

TEST_CASE("Report 3 updates with multiple panels") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pFake)), std::move(pDevice), nullptr, 0);

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x11, 0x01});

    string sError;
    poll->PollUSBData(0);

    CHECK(conn.GetInputState() == 0x0111);
}

TEST_CASE("Report 3 fires input state callback on change") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    int iCallbackCount = 0;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pFake)), std::move(pDevice),
                          [&](int) { iCallbackCount++; }, 0);

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});
    poll->PollUSBData(0);
    CHECK(iCallbackCount == 1);

    // Same state again should NOT fire.
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});
    poll->PollUSBData(0);
    CHECK(iCallbackCount == 1);
}

TEST_CASE("Report 3 always-fire mode fires on duplicate state") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    int iCallbackCount = 0;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pFake)), std::move(pDevice),
                          [&](int) { iCallbackCount++; }, 0);
    conn.SetAlwaysFireInputCallback(true);

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});

    poll->PollUSBData(0);

    CHECK(iCallbackCount == 2);
}

TEST_CASE("PollUSBData returns false when no data") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pFake)), std::move(pDevice), nullptr, 0);

    CHECK_FALSE(poll->PollUSBData(0));
    CHECK_FALSE(conn.HasReadError());
}

TEST_CASE("Read error propagates") {
    class ErrorDevice : public IHIDDevice {
    public:
        int Read(uint8_t *, size_t) override { return -1; }
        int ReadTimeout(uint8_t *, size_t, int) override { return -1; }
        int Write(const uint8_t *, size_t) override { return 0; }
        void Close() override {}
    };

    SMXDeviceConnection conn;
    // The read error must come from the read handle (the poll handle reads it).
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new ErrorDevice()), unique_ptr<IHIDDevice>(new ErrorDevice()), nullptr, 0);

    poll->PollUSBData(0);
    CHECK(conn.HasReadError());
}

TEST_CASE("Close resets state") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pFake)), std::move(pDevice), nullptr, 0);

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0xFF, 0x00});
    string sError;
    poll->PollUSBData(0);
    CHECK(conn.GetInputState() == 0x00FF);

    conn.Close();
    CHECK_FALSE(conn.IsConnected());
    CHECK(conn.GetInputState() == 0);
}

// =========================================================================
// Report 6 fragmentation and reassembly
// =========================================================================

TEST_CASE("Report 6 single packet with START|END is queued as complete packet") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    // START|END flags, payload = "AB"
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND, {'A', 'B'}));

    string sError;
    poll->PollUSBData(0);
    conn.Update(sError);

    string out;
    CHECK(conn.ReadPacket(out));
    CHECK(out == "AB");
    CHECK_FALSE(conn.ReadPacket(out));
}

TEST_CASE("Report 6 multi-fragment reassembly") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    // Fragment 1: START
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND, {'H', 'e', 'l'}));
    // Fragment 2: middle (no flags)
    pFake->QueueRead(MakeReport6(0, {'l', 'o'}));
    // Fragment 3: END
    pFake->QueueRead(MakeReport6(PACKET_FLAG_END_OF_COMMAND, {' ', 'W'}));

    string sError;
    poll->PollUSBData(0);
    conn.Update(sError);

    string out;
    CHECK(conn.ReadPacket(out));
    CHECK(out == "Hello W");
}

TEST_CASE("Report 6 START clears partial buffer") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    // Partial fragment (no END)
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND, {'o', 'l', 'd'}));

    string sError;
    poll->PollUSBData(0);
    conn.Update(sError);

    // New START should clear the old partial data
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND, {'n', 'e', 'w'}));
    pFake->QueueRead(MakeReport6(PACKET_FLAG_END_OF_COMMAND, {'!'}));

    poll->PollUSBData(0);
    conn.Update(sError);

    string out;
    CHECK(conn.ReadPacket(out));
    CHECK(out == "new!");
}

TEST_CASE("Report 6 packets not queued when inactive") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    // NOT calling conn.SetActive(true)

    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND, {'X'}));

    string sError;
    poll->PollUSBData(0);
    conn.Update(sError);

    string out;
    CHECK_FALSE(conn.ReadPacket(out));
}

// =========================================================================
// Device info parsing and connection state machine
// =========================================================================

TEST_CASE("Open queues device info request and IsConnectedWithDeviceInfo is false") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);

    CHECK(conn.IsConnected());
    CHECK_FALSE(conn.IsConnectedWithDeviceInfo());
}

TEST_CASE("Device info handshake sets IsConnectedWithDeviceInfo") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);

    CompleteDeviceInfoHandshake(conn, poll.get(), pFake,'0', 5);

    CHECK(conn.IsConnectedWithDeviceInfo());
}

TEST_CASE("Device info parses player 1 correctly") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);

    CompleteDeviceInfoHandshake(conn, poll.get(), pFake,'0', 7);

    SMXDeviceInfo info = conn.GetDeviceInfo();
    CHECK_FALSE(info.m_bP2);
    CHECK(info.m_iFirmwareVersion == 7);
}

TEST_CASE("Device info parses player 2 correctly") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);

    // player == '1' means P2
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake,'1', 3);

    SMXDeviceInfo info = conn.GetDeviceInfo();
    CHECK(info.m_bP2);
    CHECK(info.m_iFirmwareVersion == 3);
}

TEST_CASE("Device info parses serial number") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);

    CompleteDeviceInfoHandshake(conn, poll.get(), pFake,'0', 5);

    SMXDeviceInfo info = conn.GetDeviceInfo();
    // Serial bytes 0x01..0x10 → hex "0102030405060708090a0b0c0d0e0f10"
    CHECK(string(info.m_Serial) == "0102030405060708090a0b0c0d0e0f10");
}

TEST_CASE("Device info response without pending command is ignored") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);

    // Send the device info request first
    string sError;
    conn.Update(sError);

    // Complete the handshake normally
    uint8_t serial[SERIAL_SIZE] = {};
    auto payload = MakeDeviceInfoPayload('0', 5, serial);
    pFake->QueueRead(MakeReport6(PACKET_FLAG_DEVICE_INFO, payload));
    poll->PollUSBData(0);
    conn.Update(sError);
    CHECK(conn.IsConnectedWithDeviceInfo());

    // Now send another device info response with no command in flight — should be ignored
    auto payload2 = MakeDeviceInfoPayload('1', 99, serial);
    pFake->QueueRead(MakeReport6(PACKET_FLAG_DEVICE_INFO, payload2));
    poll->PollUSBData(0);
    conn.Update(sError);

    // Should still have original info
    SMXDeviceInfo info = conn.GetDeviceInfo();
    CHECK_FALSE(info.m_bP2);
    CHECK(info.m_iFirmwareVersion == 5);
}

// =========================================================================
// Command send/response flow and timeouts
// =========================================================================

TEST_CASE("SendCommand writes fragmented HID packets") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    conn.SendCommand("test");

    string sError;
    conn.Update(sError);

    // Update() only hands the packets to the writer thread; settle it before asserting
    // on what actually reached the device.
    REQUIRE(conn.WaitWritesIdle(2.0));

    // Should have written: device info request (from Open) + our command
    // Device info was already sent during handshake, so writes should include our command
    const auto &writes = pFake->GetWrites();
    REQUIRE(writes.size() >= 2);  // device info + our command

    // Last write should be our command packet
    const auto &cmdPkt = writes.back();
    CHECK(cmdPkt[0] == HID_REPORT_COMMAND);   // report ID
    CHECK(cmdPkt[1] == (PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND)); // short command fits in one packet
    CHECK(cmdPkt[2] == 4);   // payload size = "test"
    CHECK(cmdPkt[3] == 't');
    CHECK(cmdPkt[4] == 'e');
    CHECK(cmdPkt[5] == 's');
    CHECK(cmdPkt[6] == 't');
}

TEST_CASE("SendCommand callback fires on HOST_CMD_FINISHED") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    string sResponse;
    conn.SendCommand("G", [&](string r) { sResponse = r; });

    string sError;
    conn.Update(sError);  // sends the command

    // Device responds with HOST_CMD_FINISHED | START | END
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED, {'G', 0x05, 'c', 'f', 'g'}));
    poll->PollUSBData(0);
    conn.Update(sError);

    CHECK(sResponse == "G\x05""cfg");
}

TEST_CASE("Commands are serialized - second waits for first") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    string sResp1, sResp2;
    conn.SendCommand("A", [&](string r) { sResp1 = r; });
    conn.SendCommand("B", [&](string r) { sResp2 = r; });

    string sError;
    conn.Update(sError);  // hands command A to the writer thread
    REQUIRE(conn.WaitWritesIdle(2.0));

    size_t writesAfterFirst = pFake->GetWrites().size();

    conn.Update(sError);  // command A still in flight, B not handed over yet
    REQUIRE(conn.WaitWritesIdle(2.0));
    CHECK(pFake->GetWrites().size() == writesAfterFirst);

    // Complete command A
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED, {'a'}));
    poll->PollUSBData(0);
    conn.Update(sError);
    CHECK(sResp1 == "a");

    // Now B should be sent
    conn.Update(sError);
    REQUIRE(conn.WaitWritesIdle(2.0));
    CHECK(pFake->GetWrites().size() > writesAfterFirst);
}

TEST_CASE("Pending command callback does not fire without response") {
    // We need a fake GetMonotonicTime to test timeouts.
    // Since GetMonotonicTime uses a real clock, we can't easily fake it.
    // Instead, verify the timeout logic structurally: send a command,
    // don't respond, and verify it stays in flight.
    bool bCallbackFired = false;
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    conn.SendCommand("X", [&](string) { bCallbackFired = true; });

    string sError;
    conn.Update(sError);  // sends command

    // Multiple updates without response — command stays in flight
    conn.Update(sError);
    conn.Update(sError);
    CHECK_FALSE(bCallbackFired);
}

TEST_CASE("Close invokes pending command callbacks with empty string") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    string sResp1 = "not_called";
    string sResp2 = "not_called";
    conn.SendCommand("A", [&](string r) { sResp1 = r; });
    conn.SendCommand("B", [&](string r) { sResp2 = r; });

    string sError;
    conn.Update(sError);  // sends A, B is queued

    conn.Close();

    // Both callbacks should have been invoked with empty string
    CHECK(sResp1.empty());
    CHECK(sResp2.empty());
}

TEST_CASE("Write error invokes callback and reports error") {
    class FailWriteDevice : public IHIDDevice {
    public:
        int m_iWriteCount = 0;
        queue<vector<uint8_t>> m_aReads;
        void QueueRead(vector<uint8_t> pkt) { m_aReads.push(std::move(pkt)); }
        int Read(uint8_t *buf, size_t len) override {
            if(m_aReads.empty()) return 0;
            auto &pkt = m_aReads.front();
            size_t n = min(len, pkt.size());
            memcpy(buf, pkt.data(), n);
            m_aReads.pop();
            return static_cast<int>(n);
        }
        int ReadTimeout(uint8_t *buf, size_t len, int) override { return Read(buf, len); }
        int Write(const uint8_t *, size_t) override {
            m_iWriteCount++;
            // Succeed for device info request, fail for subsequent commands
            return m_iWriteCount > 1 ? -1 : 64;
        }
        void Close() override {}
    };

    auto pFake = new FailWriteDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);

    // Complete device info handshake (first write succeeds)
    string sError;
    conn.Update(sError);  // sends device info request

    uint8_t serial[SERIAL_SIZE] = {};
    auto payload = MakeDeviceInfoPayload('0', 5, serial);
    pFake->QueueRead(MakeReport6(PACKET_FLAG_DEVICE_INFO, payload));
    poll->PollUSBData(0);
    conn.Update(sError);
    REQUIRE(conn.IsConnectedWithDeviceInfo());

    conn.SetActive(true);

    string sResp = "not_called";
    conn.SendCommand("X", [&](string r) { sResp = r; });

    sError.clear();
    conn.Update(sError);  // hands the command to the writer thread

    // The write now happens off-thread, so this Update() succeeds: the failure lands on
    // the writer, which sets the write-error flag. Settle it, then the next Update()
    // surfaces the disconnect, exactly as a poll-thread read error does.
    REQUIRE(conn.WaitWritesIdle(2.0));
    CHECK(conn.HasWriteError());

    sError.clear();
    conn.Update(sError);
    CHECK_FALSE(sError.empty());

    // The manager responds to that error by closing the device, which cancels the
    // in-flight command with an empty response.
    conn.Close();
    CHECK(sResp.empty());
}

// =========================================================================
// Unsolicited HOST_CMD_FINISHED (no command in flight)
// =========================================================================

TEST_CASE("Unsolicited HOST_CMD_FINISHED does not crash") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake);
    conn.SetActive(true);

    // Send HOST_CMD_FINISHED with no command in flight
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED, {'Z', 'z'}));

    string sError;
    poll->PollUSBData(0);
    conn.Update(sError);

    CHECK(sError.empty());
    // The packet should still be queued as a read buffer (END flag set)
    string out;
    CHECK(conn.ReadPacket(out));
    CHECK(out == "Zz");
}

// =========================================================================
// Multiple Report 3 packets in single PollUSBData call
// =========================================================================

TEST_CASE("Multiple Report 3 packets in single PollUSBData retains final state") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    int iCallbackCount = 0;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pFake)), std::move(pDevice),
                          [&](int) { iCallbackCount++; }, 0);

    // Queue multiple Report 3 packets
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});  // state = 0x0001
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x03, 0x00});  // state = 0x0003
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0xFF, 0x01});  // state = 0x01FF

    poll->PollUSBData(0);

    CHECK(conn.GetInputState() == 0x01FF);
    CHECK(iCallbackCount == 3);  // each change fires callback
}

TEST_CASE("Multiple Report 3 with duplicates only fires on changes") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    int iCallbackCount = 0;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pFake)), std::move(pDevice),
                          [&](int) { iCallbackCount++; }, 0);

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});  // state = 0x0001 (change)
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});  // same (no change)
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x02, 0x00});  // state = 0x0002 (change)
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x02, 0x00});  // same (no change)

    poll->PollUSBData(0);

    CHECK(conn.GetInputState() == 0x0002);
    CHECK(iCallbackCount == 2);
}

// =========================================================================
// Move semantics
// =========================================================================

TEST_CASE("Move constructor transfers connection state") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake,'1', 7);

    // Set some input state
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x42, 0x00});
    string sError;
    poll->PollUSBData(0);

    // Move construct
    SMXDeviceConnection moved(std::move(conn));

    CHECK(moved.IsConnected());
    CHECK(moved.IsConnectedWithDeviceInfo());
    CHECK(moved.GetInputState() == 0x0042);
    CHECK(moved.GetPath() == "/fake/path");

    SMXDeviceInfo info = moved.GetDeviceInfo();
    CHECK(info.m_bP2 == true);
    CHECK(info.m_iFirmwareVersion == 7);

    // Original should be empty
    CHECK_FALSE(conn.IsConnected());
}

TEST_CASE("Move assignment transfers connection state") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);
    CompleteDeviceInfoHandshake(conn, poll.get(), pFake,'0', 3);

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x10, 0x00});
    string sError;
    poll->PollUSBData(0);

    SMXDeviceConnection moved;
    moved = std::move(conn);

    CHECK(moved.IsConnected());
    CHECK(moved.IsConnectedWithDeviceInfo());
    CHECK(moved.GetInputState() == 0x0010);
    CHECK(moved.GetPath() == "/fake/path");

    SMXDeviceInfo info = moved.GetDeviceInfo();
    CHECK_FALSE(info.m_bP2);
    CHECK(info.m_iFirmwareVersion == 3);

    CHECK_FALSE(conn.IsConnected());
}

TEST_CASE("Close clears read error flag for reconnection") {
    class FailOnceDevice : public IHIDDevice {
    public:
        int Read(uint8_t *, size_t) override { return -1; }
        int ReadTimeout(uint8_t *, size_t, int) override { return -1; }
        int Write(const uint8_t *, size_t) override { return 64; }
        void Close() override {}
    };

    SMXDeviceConnection conn;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FailOnceDevice()), unique_ptr<IHIDDevice>(new FailOnceDevice()), nullptr, 0);

    // Trigger a read error
    poll->PollUSBData(0);
    CHECK(conn.HasReadError());

    // Close should clear the error flag
    conn.Close();
    CHECK_FALSE(conn.HasReadError());

    // Reopen with a working device — Update should not immediately error
    auto pFake = new FakeHIDDevice();
    OpenSplit(conn, pFake, "/fake/path2");

    string sError;
    conn.Update(sError);
    CHECK(sError.empty());
}

TEST_CASE("Lights commands are tagged for backlog bounding; sensor/config are not") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    auto poll = OpenSplit(conn, pFake);

    // Only the auto device-info request is queued so far (not a lights command).
    CHECK_FALSE(conn.HasUnsentLights());

    conn.SendCommandLights("4aaa\n");
    CHECK(conn.HasUnsentLights());

    auto pFake2 = new FakeHIDDevice();
    SMXDeviceConnection conn2;
    OpenSplit(conn2, pFake2, "/fake/path2");
    conn2.SendCommand("y1\n");
    conn2.SendCommandPriority("y1\n");
    CHECK_FALSE(conn2.HasUnsentLights());
}

// =========================================================================
// Writer thread: writes happen off the caller's thread
// =========================================================================

// A write handle that blocks until released, so the test can observe that Update()
// returned while a write was still on the wire.
namespace {
class BlockingWriteDevice : public IHIDDevice
{
public:
    int Read(uint8_t *, size_t) override { return 0; }
    int ReadTimeout(uint8_t *, size_t, int) override { return 0; }
    int Write(const uint8_t *, size_t len) override
    {
        m_bInWrite.store(true);
        while(!m_bRelease.load())
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        return static_cast<int>(len);
    }
    void Close() override {}

    std::atomic<bool> m_bInWrite{false};
    std::atomic<bool> m_bRelease{false};
};
} // anonymous namespace

TEST_CASE("Update does not block on the wire") {
    auto pBlocking = new BlockingWriteDevice();
    auto pRead = new FakeHIDDevice();

    SMXDeviceConnection conn;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pRead)),
                          unique_ptr<IHIDDevice>(pBlocking), nullptr, 0);

    // Open() queues the device-info request. Update() hands it to the writer, which
    // parks inside Write(); Update() itself must return regardless.
    string sError;
    conn.Update(sError);
    CHECK(sError.empty());

    // The writer really is stuck mid-write, so the handoff was genuinely asynchronous.
    bool bReachedWrite = false;
    for(int i = 0; i < 5000 && !bReachedWrite; ++i)
    {
        bReachedWrite = pBlocking->m_bInWrite.load();
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    CHECK(bReachedWrite);
    CHECK_FALSE(conn.WaitWritesIdle(0.01));  // still in flight

    pBlocking->m_bRelease.store(true);
    CHECK(conn.WaitWritesIdle(2.0));
}

TEST_CASE("A command is not marked sent until its packets are on the wire") {
    auto pBlocking = new BlockingWriteDevice();
    auto pRead = new FakeHIDDevice();

    SMXDeviceConnection conn;
    auto poll = conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FakeHIDView(pRead)),
                          unique_ptr<IHIDDevice>(pBlocking), nullptr, 0);

    string sError;
    conn.Update(sError);            // hands the device-info request to the writer

    // While the write is parked, repeated Update()s must not promote the command to
    // sent: its response timeout only starts once the packets reach the wire.
    conn.Update(sError);
    CHECK(sError.empty());
    CHECK_FALSE(conn.WaitWritesIdle(0.01));

    pBlocking->m_bRelease.store(true);
    REQUIRE(conn.WaitWritesIdle(2.0));
    conn.Update(sError);            // now promotes to sent
    CHECK(sError.empty());
}
