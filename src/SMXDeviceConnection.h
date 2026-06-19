#ifndef SMXDeviceConnection_h
#define SMXDeviceConnection_h

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "SMXHIDInterface.h"

// USB report flags used in the SMX protocol for packet fragmentation and control.
#define PACKET_FLAG_START_OF_COMMAND   0x04  // Indicates start of a multi-packet command
#define PACKET_FLAG_END_OF_COMMAND     0x01  // Indicates end of a multi-packet command
#define PACKET_FLAG_HOST_CMD_FINISHED  0x02  // Device has finished processing command
#define PACKET_FLAG_DEVICE_INFO        0x80  // This packet contains device info response

// HID report IDs used in the SMX protocol.
static constexpr uint8_t HID_REPORT_INPUT_STATE = 0x03;  // Input state (panel presses)
static constexpr uint8_t HID_REPORT_COMMAND     = 0x05;  // Outgoing commands to device
static constexpr uint8_t HID_REPORT_DATA        = 0x06;  // Incoming data/config from device

// HID packet sizing.
static constexpr size_t HID_PACKET_SIZE      = 64;  // Total HID packet size in bytes
static constexpr size_t HID_MAX_PAYLOAD_SIZE = 61;  // Max payload per packet (64 - 3 byte header)

// Command timeout.
static constexpr double COMMAND_TIMEOUT_SECONDS = 2.0;  // Seconds before retrying a command

namespace SMX {

/// Immutable device information retrieved from the hardware on connection.
/// This struct holds the device metadata that doesn't change during normal operation.
struct SMXDeviceInfo
{
    /// True if this device's physical jumper is set to Player 2 mode.
    bool m_bP2 = false;

    /// Serial number as a null-terminated hex string (32 chars + null terminator).
    /// Format: 32 lowercase hex characters representing 16 bytes of device serial.
    char m_Serial[33] = {};

    /// Device firmware version number.
    uint16_t m_iFirmwareVersion = 0;
};

/// Cross-thread state for one connection, held by std::shared_ptr so both the
/// read side (SMXPollHandle, on a per-pad poll thread) and the command side
/// (SMXDeviceConnection, on the main I/O thread) can reference it independently.
/// This mirrors the Arc<ConnShared> in the Rust SDK: the poll thread writes the
/// input state, read-error flag, and Report 6 buffer; the main thread reads them.
/// Because the shared object lives on the heap and outlives a pad-slot swap, the
/// poll thread keeps reading its own device across a swap with no rebinding.
struct SMXConnectionShared
{
    /// Current panel press bitmask. Written by the poll thread, read anywhere.
    std::atomic<uint16_t> m_iInputState{0};

    /// Fire the input callback on every Report 3 packet (true) or only on change
    /// (false, default). Written by the main thread, read by the poll thread.
    std::atomic<bool> m_bAlwaysFireInputCallback{false};

    /// Set when a read fails; the main thread polls this to close the device.
    std::atomic<bool> m_bHadReadError{false};

    /// Slot index the input callback reports. Updated on a pad swap (instead of
    /// rebinding the callback), so the poll thread reads it lock-free.
    std::atomic<int> m_iPadIndex{0};

    /// Raw Report 6 packets accumulated by the poll thread for the main thread.
    std::mutex m_Report6BufferMutex;
    std::string m_sReport6Buffer;  // guarded by m_Report6BufferMutex

    /// Fired from the poll thread on an input change (and on every packet when
    /// always-fire is set). Installed once at Open() and never rebound; receives
    /// the current pad index (m_iPadIndex) as its argument.
    std::function<void(int pad)> m_pInputStateChangedCallback;
};

/// Read side of a connection, owned by a per-pad poll thread (mirrors Rust's
/// PollHandle). Owns the read HID handle for the connection's lifetime and shares
/// SMXConnectionShared with the command-side SMXDeviceConnection, so input reads
/// run entirely off any manager lock and independently of the other pad.
class SMXPollHandle
{
public:
    SMXPollHandle(std::unique_ptr<IHIDDevice> pReadDevice, std::shared_ptr<SMXConnectionShared> pShared):
        m_pReadDevice(std::move(pReadDevice)), m_pShared(std::move(pShared)) {}

    /// Interrupt-driven poll: the first read blocks up to iFirstReadTimeoutMs (the
    /// kernel wakes the caller the instant a report arrives), then the rest of the
    /// OS read buffer is drained non-blocking. Report 3 (input) is parsed inline
    /// and fires the input callback; Report 6 (command/config) is buffered for the
    /// main thread. Returns true if Report 6 data was buffered.
    bool PollUSBData(int iFirstReadTimeoutMs);

    /// True if a read failed; the main thread checks this to close the device.
    bool HasReadError() const { return m_pShared->m_bHadReadError.load(std::memory_order_relaxed); }

private:
    std::unique_ptr<IHIDDevice> m_pReadDevice;   // Reads only (this poll thread)
    std::shared_ptr<SMXConnectionShared> m_pShared;
};

/// Low-level USB communication abstraction for a single StepManiaX device.
///
/// This class handles:
/// - Opening HID connections to the device
/// - Sending commands and receiving responses asynchronously
/// - Reading input state (pressed panels) from the device
/// - Requesting and parsing device information
/// - Buffering and fragmenting data across HID packets (64 bytes max)
///
/// The class is non-copyable but movable to support device reordering in the manager.
/// All operations are nonblocking; commands are queued and processed in the background.
///
/// ============================================================================
/// THREADING MODEL: Split read/command sides for low-latency input
/// ============================================================================
///
/// A connection is split across two objects that share SMXConnectionShared:
///
/// 1. SMXPollHandle (read side, one per-pad poll thread):
///    - Blocks on the read handle (ReadTimeout) and wakes the instant a report
///      arrives, then drains the rest non-blocking. See PollUSBData().
///    - Parses Report 3 (input) inline, updates m_iInputState atomically, and
///      fires the input callback; buffers Report 6 into m_sReport6Buffer.
///    - Owns the read handle for the connection's lifetime; runs off any manager
///      lock, so the two pads never block on each other and a slow USB write on
///      the main thread can't stall input. Spawned/reaped by SMXManager.
///
/// 2. SMXDeviceConnection (command side, main I/O thread, holds m_pLock):
///    - CheckReads() drains Report 6 from m_sReport6Buffer; handles fragmentation,
///      command callbacks, timeouts. Never touches Report 3 or m_iInputState.
///    - Owns the write handle; queues and sends commands. See ThreadMain().
///
/// Cross-thread state lives in SMXConnectionShared (held by shared_ptr by both
/// objects): m_iInputState / m_bHadReadError / m_iPadIndex (atomics),
/// m_sReport6Buffer (m_Report6BufferMutex), and the input callback (installed
/// once at Open, never rebound). Main-thread-only state (write handle, path,
/// device info, read/command buffers) needs no synchronization.
///
/// PROTOCOL DETAILS:
///
///   Report 3 (Input State): 3 bytes [ID=3][low byte][high byte]
///   - Parsed inline in SMXPollHandle::PollUSBData(), never buffered
///   - Updates m_iInputState atomically with full 16-bit value
///   - Bit layout: 0-8 = panels, 9-15 = unused
///
///   Report 6 (Commands/Config): [ID=6][flags][size][payload...]
///   - Variable length: 3-byte header + 0-61 bytes payload
///   - Fragmentation flags (cf. PACKET_FLAG_*):
///     • 0x04 (START_OF_COMMAND): clears buffered fragment
///     • 0x01 (END_OF_COMMAND): queues complete packet to m_sReadBuffers
///     • 0x02 (HOST_CMD_FINISHED): invokes command callback
///   - Buffered in m_sReport6Buffer by the poll thread, processed by main thread
///
class SMXDeviceConnection
{
public:
    SMXDeviceConnection();
    ~SMXDeviceConnection();

    // Non-copyable (prevents accidental duplicate connections)
    SMXDeviceConnection(const SMXDeviceConnection &) = delete;
    SMXDeviceConnection &operator=(const SMXDeviceConnection &) = delete;

    // Movable (required for device reordering by pad index)
    SMXDeviceConnection(SMXDeviceConnection &&other) noexcept;
    SMXDeviceConnection &operator=(SMXDeviceConnection &&other) noexcept;

    /// Opens a HID connection using two independent handles to the same device:
    /// one for reads (the per-pad poll thread) and one for writes (main I/O
    /// thread), so a read never waits behind a blocking write. Creates the shared
    /// state, stores the write handle, automatically requests device info, and
    /// returns the read-side poll handle for the manager to run on a poll thread.
    /// @param sPath HID device path string (stored for identification).
    /// @param pReadDevice Opened HID handle owned by the returned poll handle.
    /// @param pWriteDevice Opened HID handle used only by CheckWrites (writes).
    /// @param inputChangedCb Fired from the poll thread on input change, with the
    ///        current pad index; installed once and never rebound.
    /// @param iPadIndex Initial slot index the input callback reports.
    /// @return The read-side poll handle on success, or null on failure.
    std::unique_ptr<SMXPollHandle> Open(const std::string &sPath,
        std::unique_ptr<IHIDDevice> pReadDevice, std::unique_ptr<IHIDDevice> pWriteDevice,
        std::function<void(int pad)> inputChangedCb, int iPadIndex);

    /// Closes the connection and cancels all pending commands.
    /// Invokes completion callbacks with empty strings to notify of cancellation.
    void Close();

    /// Returns true if the HID connection is open (though device info may not be retrieved yet).
    bool IsConnected() const { return m_pWriteDevice != nullptr; }

    /// Returns true if the connection is open AND device info has been received.
    bool IsConnectedWithDeviceInfo() const { return m_pWriteDevice != nullptr && m_bGotInfo; }

    /// Returns the HID device path.
    const std::string &GetPath() const { return m_sPath; }

    /// Retrieves the cached device information.
    /// Only valid after IsConnectedWithDeviceInfo() returns true.
    const SMXDeviceInfo &GetDeviceInfo() const { return m_DeviceInfo; }

    /// Processes I/O operations. Called once per frame from the I/O thread.
    /// Performs nonblocking reads from the HID device, writes pending commands,
    /// and handles command timeouts.
    /// @param sError [out] Error message if an error occurs.
    void Update(std::string &sError);

    /// Sets whether the device should actively send input state updates.
    /// When active, the device continuously sends input packets; when inactive,
    /// it only responds to commands.
    void SetActive(const bool bActive) { m_bActive = bActive; }

    /// Returns whether the device is actively sending input updates.
    bool GetActive() const { return m_bActive; }

    /// Reads a completed packet from the internal buffer.
    /// Packets are queued as they are fully received from the device.
    /// @param out [out] String containing the packet data if available.
    /// @return True if a packet was available and has been dequeued, false if empty.
    bool ReadPacket(std::string &out);

    /// Queues a command to be sent to the device asynchronously.
    /// The command is automatically fragmented into 64-byte HID packets.
    /// @param cmd Command string to send.
    /// @param pComplete Optional callback invoked when the device responds (or on error).
    void SendCommand(const std::string &cmd, std::function<void(std::string response)> pComplete = nullptr);

    /// Like SendCommand, but queues at the FRONT so the command is sent ahead of
    /// already-queued commands (after any in-flight one finishes).
    ///
    /// Used for latency-sensitive requests (sensor-test polling) so they don't
    /// wait behind a queued light frame. Lights are coalesced and bounded (see
    /// SendCommandLights / HasUnsentLights) and the sensor request is paced, so
    /// this no longer starves the light stream.
    void SendCommandPriority(const std::string &cmd, std::function<void(std::string response)> pComplete = nullptr);

    /// Queue a panel-lights command. Tagged so the manager can keep at most one
    /// light frame queued at a time (HasUnsentLights), since lights are
    /// last-writer-wins state and stale frames must never back up behind a
    /// prioritized sensor request.
    void SendCommandLights(const std::string &cmd);

    /// True if any un-sent panel-lights command is queued (not counting one
    /// already in flight). Used to avoid piling new light frames onto the queue.
    bool HasUnsentLights() const;

    /// Retrieves the current input state (pressed panels) bitmask.
    uint16_t GetInputState() const { return m_pShared ? m_pShared->m_iInputState.load() : 0; }

    /// Sets whether the input state callback fires on every Report 3 packet (true)
    /// or only when the state actually changes (false, default).
    void SetAlwaysFireInputCallback(bool b) { if(m_pShared) m_pShared->m_bAlwaysFireInputCallback.store(b, std::memory_order_relaxed); }

    /// Returns true if the poll thread encountered a read error.
    /// The main thread checks this to trigger device disconnect.
    bool HasReadError() const { return m_pShared && m_pShared->m_bHadReadError.load(std::memory_order_relaxed); }

    /// Updates the slot index the input callback reports. Called on a pad swap so
    /// the running poll thread attributes input to the new slot without rebinding
    /// the callback. No-op before Open.
    void SetSharedPadIndex(int i) { if(m_pShared) m_pShared->m_iPadIndex.store(i, std::memory_order_relaxed); }

private:
    /// Sends a device info request packet to the device.
    /// The response is handled asynchronously in HandleUsbPacket() and sets m_bGotInfo.
    /// @param pComplete Optional callback for the device info response.
    void RequestDeviceInfo(std::function<void(std::string response)> pComplete = nullptr);

    /// Processes all available data from the HID device.
    /// Reads packets until no more data is available, handling command timeouts.
    /// @param sError [out] Error message if a read fails.
    void CheckReads();

    /// Sends the next pending command to the device if no command is currently in flight.
    /// Breaks the command into 64-byte HID packets and sends them sequentially.
    /// @param sError [out] Error message if a write fails.
    void CheckWrites(std::string &sError);

    /// Processes a single Report 6 USB packet received from the device.
    /// Handles command/config packets with fragmentation flags.
    /// @param pData Pointer to packet data (flags byte at offset 0, not report ID).
    /// @param iLen Total length of the packet (header + payload).
    void HandleUsbPacket(const char *pData, size_t iLen);

    // --- Connection state ---
    // Cross-thread state (input bitmask, read-error flag, pad index, Report 6
    // buffer, input callback) shared with the read-side SMXPollHandle. Created by
    // Open(), released by Close(); null while disconnected. The poll thread holds
    // its own shared_ptr to the same object, so it stays valid across a pad swap.
    std::shared_ptr<SMXConnectionShared> m_pShared;
    // Write handle (main I/O thread only). The read handle lives in the poll
    // handle, not here. Set by Open(), reset by Close().
    std::unique_ptr<IHIDDevice> m_pWriteDevice;  // Writes only (main I/O thread)
    std::string m_sPath;                    // HID device path for identification
    bool m_bActive = false;                 // True after activation command sent
    bool m_bGotInfo = false;                // True once device info response received

    // --- Packet reassembly (main thread, protected by external lock) ---
    std::deque<std::string> m_sReadBuffers;     // Complete packets ready for application
    std::string m_sCurrentReadBuffer;           // Fragment accumulation for Report 6

    // --- Device metadata ---
    SMXDeviceInfo m_DeviceInfo;                 // Cached device info (fw version, serial, P1/P2)

    /// Represents a command pending transmission or awaiting response.
    /// Commands may be fragmented into multiple 64-byte HID packets.
    struct PendingCommand {
        std::string sData;                                        // Raw command data (all HID packets combined)
        std::function<void(std::string response)> m_pComplete;    // Callback when response received
        bool m_bIsDeviceInfoCommand = false;                      // True if this is a device info request
        bool m_bIsLights = false;                                 // True if a panel-lights command (bounds the light backlog)
        bool m_bSent = false;                                     // True if sent to device and awaiting response
        double m_fSentAt = 0;                                     // Time when command was sent (for timeout detection)
    };

    /// Builds a PendingCommand, fragmenting cmd into 64-byte HID packets.
    /// Shared by SendCommand (queues at back) and SendCommandPriority (front).
    std::unique_ptr<PendingCommand> BuildCommand(const std::string &cmd, std::function<void(std::string response)> pComplete);

    std::deque<std::unique_ptr<PendingCommand>> m_aPendingCommands; // Queue of commands not yet sent
    std::unique_ptr<PendingCommand> m_pCurrentCommand;             // Command currently awaiting response
};

}

#endif
