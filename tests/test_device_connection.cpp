#include <doctest/doctest.h>
#include "SMX.h"
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
static void CompleteDeviceInfoHandshake(SMXDeviceConnection &conn, FakeHIDDevice *pFake,
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
    conn.PollUSBData();

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
    conn.Open("/fake/path", std::move(pDevice));

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x10, 0x00});

    conn.PollUSBData();

    CHECK(conn.GetInputState() == 0x0010);
}

TEST_CASE("Report 3 updates with multiple panels") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x11, 0x01});

    string sError;
    conn.PollUSBData();

    CHECK(conn.GetInputState() == 0x0111);
}

TEST_CASE("Report 3 fires input state callback on change") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    int iCallbackCount = 0;
    conn.SetInputStateChangedCallback([&]() { iCallbackCount++; });

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});
    string sError;
    conn.PollUSBData();
    CHECK(iCallbackCount == 1);

    // Same state again — should NOT fire
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});
    conn.PollUSBData();
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

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});

    string sError;
    conn.PollUSBData();

    CHECK(iCallbackCount == 2);
}

TEST_CASE("PollUSBData returns false when no data") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    CHECK_FALSE(conn.PollUSBData());
    CHECK_FALSE(conn.HasReadError());
}

TEST_CASE("Read error propagates") {
    class ErrorDevice : public IHIDDevice {
    public:
        int Read(uint8_t *, size_t) override { return -1; }
        int Write(const uint8_t *, size_t) override { return 0; }
        void Close() override {}
    };

    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(new ErrorDevice()));

    conn.PollUSBData();
    CHECK(conn.HasReadError());
}

TEST_CASE("Close resets state") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0xFF, 0x00});
    string sError;
    conn.PollUSBData();
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
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
    conn.SetActive(true);

    // START|END flags, payload = "AB"
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND, {'A', 'B'}));

    string sError;
    conn.PollUSBData();
    conn.Update(sError);

    string out;
    CHECK(conn.ReadPacket(out));
    CHECK(out == "AB");
    CHECK_FALSE(conn.ReadPacket(out));
}

TEST_CASE("Report 6 multi-fragment reassembly") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
    conn.SetActive(true);

    // Fragment 1: START
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND, {'H', 'e', 'l'}));
    // Fragment 2: middle (no flags)
    pFake->QueueRead(MakeReport6(0, {'l', 'o'}));
    // Fragment 3: END
    pFake->QueueRead(MakeReport6(PACKET_FLAG_END_OF_COMMAND, {' ', 'W'}));

    string sError;
    conn.PollUSBData();
    conn.Update(sError);

    string out;
    CHECK(conn.ReadPacket(out));
    CHECK(out == "Hello W");
}

TEST_CASE("Report 6 START clears partial buffer") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
    conn.SetActive(true);

    // Partial fragment (no END)
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND, {'o', 'l', 'd'}));

    string sError;
    conn.PollUSBData();
    conn.Update(sError);

    // New START should clear the old partial data
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND, {'n', 'e', 'w'}));
    pFake->QueueRead(MakeReport6(PACKET_FLAG_END_OF_COMMAND, {'!'}));

    conn.PollUSBData();
    conn.Update(sError);

    string out;
    CHECK(conn.ReadPacket(out));
    CHECK(out == "new!");
}

TEST_CASE("Report 6 packets not queued when inactive") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
    // NOT calling conn.SetActive(true)

    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND, {'X'}));

    string sError;
    conn.PollUSBData();
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
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));

    CHECK(conn.IsConnected());
    CHECK_FALSE(conn.IsConnectedWithDeviceInfo());
}

TEST_CASE("Device info handshake sets IsConnectedWithDeviceInfo") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));

    CompleteDeviceInfoHandshake(conn, pFake, '0', 5);

    CHECK(conn.IsConnectedWithDeviceInfo());
}

TEST_CASE("Device info parses player 1 correctly") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));

    CompleteDeviceInfoHandshake(conn, pFake, '0', 7);

    SMXDeviceInfo info = conn.GetDeviceInfo();
    CHECK_FALSE(info.m_bP2);
    CHECK(info.m_iFirmwareVersion == 7);
}

TEST_CASE("Device info parses player 2 correctly") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));

    // player == '1' means P2
    CompleteDeviceInfoHandshake(conn, pFake, '1', 3);

    SMXDeviceInfo info = conn.GetDeviceInfo();
    CHECK(info.m_bP2);
    CHECK(info.m_iFirmwareVersion == 3);
}

TEST_CASE("Device info parses serial number") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));

    CompleteDeviceInfoHandshake(conn, pFake, '0', 5);

    SMXDeviceInfo info = conn.GetDeviceInfo();
    // Serial bytes 0x01..0x10 → hex "0102030405060708090a0b0c0d0e0f10"
    CHECK(string(info.m_Serial) == "0102030405060708090a0b0c0d0e0f10");
}

TEST_CASE("Device info response without pending command is ignored") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));

    // Send the device info request first
    string sError;
    conn.Update(sError);

    // Complete the handshake normally
    uint8_t serial[SERIAL_SIZE] = {};
    auto payload = MakeDeviceInfoPayload('0', 5, serial);
    pFake->QueueRead(MakeReport6(PACKET_FLAG_DEVICE_INFO, payload));
    conn.PollUSBData();
    conn.Update(sError);
    CHECK(conn.IsConnectedWithDeviceInfo());

    // Now send another device info response with no command in flight — should be ignored
    auto payload2 = MakeDeviceInfoPayload('1', 99, serial);
    pFake->QueueRead(MakeReport6(PACKET_FLAG_DEVICE_INFO, payload2));
    conn.PollUSBData();
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
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
    conn.SetActive(true);

    conn.SendCommand("test");

    string sError;
    conn.Update(sError);

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
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
    conn.SetActive(true);

    string sResponse;
    conn.SendCommand("G", [&](string r) { sResponse = r; });

    string sError;
    conn.Update(sError);  // sends the command

    // Device responds with HOST_CMD_FINISHED | START | END
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED, {'G', 0x05, 'c', 'f', 'g'}));
    conn.PollUSBData();
    conn.Update(sError);

    CHECK(sResponse == "G\x05""cfg");
}

TEST_CASE("Commands are serialized - second waits for first") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
    conn.SetActive(true);

    string sResp1, sResp2;
    conn.SendCommand("A", [&](string r) { sResp1 = r; });
    conn.SendCommand("B", [&](string r) { sResp2 = r; });

    string sError;
    conn.Update(sError);  // sends command A only

    size_t writesAfterFirst = pFake->GetWrites().size();

    conn.Update(sError);  // command A still in flight, B not sent yet
    CHECK(pFake->GetWrites().size() == writesAfterFirst);

    // Complete command A
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED, {'a'}));
    conn.PollUSBData();
    conn.Update(sError);
    CHECK(sResp1 == "a");

    // Now B should be sent
    conn.Update(sError);
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
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
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
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
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
        int Write(const uint8_t *, size_t) override {
            m_iWriteCount++;
            // Succeed for device info request, fail for subsequent commands
            return m_iWriteCount > 1 ? -1 : 64;
        }
        void Close() override {}
    };

    auto pFake = new FailWriteDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));

    // Complete device info handshake (first write succeeds)
    string sError;
    conn.Update(sError);  // sends device info request

    uint8_t serial[SERIAL_SIZE] = {};
    auto payload = MakeDeviceInfoPayload('0', 5, serial);
    pFake->QueueRead(MakeReport6(PACKET_FLAG_DEVICE_INFO, payload));
    conn.PollUSBData();
    conn.Update(sError);
    REQUIRE(conn.IsConnectedWithDeviceInfo());

    conn.SetActive(true);

    string sResp = "not_called";
    conn.SendCommand("X", [&](string r) { sResp = r; });

    sError.clear();
    conn.Update(sError);  // tries to write command, fails

    CHECK_FALSE(sError.empty());
    CHECK(sResp.empty());  // callback invoked with empty string on error
}

// =========================================================================
// Unsolicited HOST_CMD_FINISHED (no command in flight)
// =========================================================================

TEST_CASE("Unsolicited HOST_CMD_FINISHED does not crash") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake);
    conn.SetActive(true);

    // Send HOST_CMD_FINISHED with no command in flight
    pFake->QueueRead(MakeReport6(PACKET_FLAG_START_OF_COMMAND | PACKET_FLAG_END_OF_COMMAND | PACKET_FLAG_HOST_CMD_FINISHED, {'Z', 'z'}));

    string sError;
    conn.PollUSBData();
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
    conn.Open("/fake/path", std::move(pDevice));

    int iCallbackCount = 0;
    conn.SetInputStateChangedCallback([&]() { iCallbackCount++; });

    // Queue multiple Report 3 packets
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});  // state = 0x0001
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x03, 0x00});  // state = 0x0003
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0xFF, 0x01});  // state = 0x01FF

    string sError;
    conn.PollUSBData();

    CHECK(sError.empty());
    CHECK(conn.GetInputState() == 0x01FF);
    CHECK(iCallbackCount == 3);  // each change fires callback
}

TEST_CASE("Multiple Report 3 with duplicates only fires on changes") {
    auto pFake = new FakeHIDDevice();
    unique_ptr<IHIDDevice> pDevice(pFake);

    SMXDeviceConnection conn;
    conn.Open("/fake/path", std::move(pDevice));

    int iCallbackCount = 0;
    conn.SetInputStateChangedCallback([&]() { iCallbackCount++; });

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});  // state = 0x0001 (change)
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x01, 0x00});  // same (no change)
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x02, 0x00});  // state = 0x0002 (change)
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x02, 0x00});  // same (no change)

    string sError;
    conn.PollUSBData();

    CHECK(conn.GetInputState() == 0x0002);
    CHECK(iCallbackCount == 2);
}

// =========================================================================
// Move semantics
// =========================================================================

TEST_CASE("Move constructor transfers connection state") {
    auto pFake = new FakeHIDDevice();
    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake, '1', 7);

    // Set some input state
    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x42, 0x00});
    string sError;
    conn.PollUSBData();

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
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(pFake));
    CompleteDeviceInfoHandshake(conn, pFake, '0', 3);

    pFake->QueueRead({HID_REPORT_INPUT_STATE, 0x10, 0x00});
    string sError;
    conn.PollUSBData();

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
        int Write(const uint8_t *, size_t) override { return 64; }
        void Close() override {}
    };

    SMXDeviceConnection conn;
    conn.Open("/fake/path", unique_ptr<IHIDDevice>(new FailOnceDevice()));

    // Trigger a read error
    conn.PollUSBData();
    CHECK(conn.HasReadError());

    // Close should clear the error flag
    conn.Close();
    CHECK_FALSE(conn.HasReadError());

    // Reopen with a working device — Update should not immediately error
    auto pFake = new FakeHIDDevice();
    conn.Open("/fake/path2", unique_ptr<IHIDDevice>(pFake));

    string sError;
    conn.Update(sError);
    CHECK(sError.empty());
}
