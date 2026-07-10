#ifndef SMXManager_h
#define SMXManager_h

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "SMX.h"
#include "SMXDevice.h"
#include "SMXHIDInterface.h"

namespace SMX {

/// Decide slot ordering from a user-pinned serial->slot assignment (the core of
/// the SMX_SetPlayerAssignment override). Returns 0 (leave) or 1 (swap) when the
/// assignment determines the order, otherwise -1 (defer to jumper ordering). Two
/// connected pads need the full serial pair; a single connected pad follows a
/// single-sided assignment (P2-only -> slot 1, P1-only -> slot 0). Declared here
/// so it can be unit-tested in isolation; not part of the public SMX_* API.
int GetOverrideSwap(const std::string asAssignment[2], const SMXInfo &info0, const SMXInfo &info1);

// ---------------------------------------------------------------------------
// SMXManager — device search, I/O thread, orchestration
// ---------------------------------------------------------------------------
// Manages the lifecycle of all connected StepManiaX devices. This class is
// responsible for:
// - Enumerating and discovering SMX devices via HID
// - Running one per-pad poll thread for low-latency, interrupt-driven input
// - Running a main I/O thread for device connections, commands, and config
// - Ensuring proper device ordering (Player 1 and Player 2)
// - Notifying the application of device state changes via callbacks

class SMXManager
{
public:
    /// Constructor initializes the manager and starts the background I/O thread.
    explicit SMXManager(const std::function<void(int, SMXUpdateCallbackReason)>& callback);

    /// Constructor that accepts a custom enumerator (for testing).
    SMXManager(const std::function<void(int, SMXUpdateCallbackReason)>& callback, std::unique_ptr<IHIDEnumerator> pEnumerator);

    ~SMXManager();

    /// Retrieves a pointer to a device by pad index (0 or 1).
    SMXDevice *GetDevice(int pad);

    /// Generates and assigns random serial numbers to all connected devices
    /// that don't already have one.
    void SetSerialNumbers();

    /// Pins pad serials to player slots (sP1Serial -> slot 0, sP2Serial -> slot 1),
    /// overriding the hardware P1/P2 jumper when ordering the two slots. The
    /// override only takes effect when both connected pads' serials are the two
    /// given serials; otherwise (no override, a single pad, or an unrecognized
    /// serial) ordering falls back to the jumper. Empty strings clear the override.
    /// Applies immediately, firing Connected callbacks for any slot that changed.
    void SetPlayerAssignment(const std::string &sP1Serial, const std::string &sP2Serial);

    /// Sets the main-thread loop cadence (milliseconds), which paces lifecycle
    /// work: enumeration, command writes, lights, and config/sensor responses.
    /// Input reads are no longer paced by a poll interval: each pad's poll thread
    /// blocks on the device and wakes the instant a report arrives.
    void SetMainThreadSleepMs(int iMainThreadMs);
    void ReenableAutoLights();

    /// Re-enables automatic panel lighting on a single pad, leaving the other pad's
    /// lighting (and any frame queued for it) untouched. Drops this pad's share of every
    /// queued lights frame first: a frame already pending would otherwise land after the
    /// command below and immediately re-disable the auto-lighting we just asked for.
    void ReenableAutoLightsForPad(int iPad);

    void SetPlatformLights(const char *pLightData);

    /// Sets panel LED colors for both pads. Data is split per-pad internally.
    void SetLights(const char *pLightData, int iLightDataSize);

    /// Sets panel LED colors for the pads selected by pads[].
    ///
    /// pLightData always covers both pads (the same layout SetLights takes); a pad whose
    /// entry in pads[] is false has its slice ignored and receives no lights command at
    /// all. Because a pad returns to its firmware auto-lighting once it stops receiving
    /// lights commands, deselecting a pad hands its LEDs back rather than lighting it
    /// black. Use ReenableAutoLightsForPad to skip the firmware's timeout.
    ///
    /// Both pads' commands are still queued together, so a frame sent to one pad never
    /// drifts out of phase with the other's.
    void SetLightsForPads(const char *pLightData, int iLightDataSize, const bool pads[2]);

    void SetPanelTestMode(PanelTestMode mode);
    void SetInputStateMode(bool bAlwaysFire);

private:
    void ThreadMain();
    void UpdatePanelTestMode();
    void AttemptConnections();
    bool CorrectDeviceOrder();
    void SendPendingLightsCommands();

    /// Body of a per-pad poll thread: block on the device (interrupt-driven),
    /// fire input callbacks inline, and wake the main thread for buffered Report 6
    /// data or a read error. Exits on stop, global shutdown, or a read error.
    void PadPollLoop(SMXPollHandle *pPoll, const std::shared_ptr<std::atomic<bool>> &pStop);

    /// Starts a poll thread for the given slot, handing it ownership of the read
    /// handle. The slot's prior thread (if any) must already be reaped.
    void SpawnPollThread(int slot, std::unique_ptr<SMXPollHandle> pPoll);

    /// Signals the slot's poll thread to stop and joins it (returns within one
    /// read timeout); a no-op if the slot has no thread.
    void StopAndJoinPollThread(int slot);

    /// A running per-pad poll thread plus the flag used to stop it. The thread
    /// owns its SMXPollHandle and does blocking reads, so each pad's input is read
    /// independently. Movable so the array can be swapped on a pad reorder.
    struct PollThread {
        std::shared_ptr<std::atomic<bool>> m_pStop;
        std::thread m_Thread;
    };

    /// A single lights command to be sent to both pads at a scheduled time.
    struct PendingLightsCommand {
        double fTimeToSend = 0;       // Monotonic time when this command should be dispatched
        std::string sPadCommand[2];   // Command string per pad (empty = skip)
    };

    // --- Synchronization and threading ---
    std::recursive_mutex m_Lock;                // Protects all mutable state below
    std::thread m_Thread;                       // Main I/O thread (connections, commands, config)
    // One poll thread per pad. Each owns its SMXPollHandle (read handle + shared
    // state) and does blocking reads entirely off m_Lock, so a slow USB write on
    // the main thread can't stall input and the two pads never block on each
    // other. Touched only by the main thread (spawn on connect, reap on read
    // error, swap on reorder) and the destructor (after the main thread joins),
    // so it needs no lock of its own. The input callback fired from a poll thread
    // must not call back into m_Lock-taking SMX_* APIs (SMX_GetInputState is
    // lock-free and safe); see the threading notes in SMXDeviceConnection.h.
    PollThread m_PollThreads[2];
    // For deadlock detection in the destructor (catches SMX_Stop() called from
    // within a callback). Atomic so they can be read without synchronization: the
    // main thread id is written once at startup; each poll thread id is written by
    // SpawnPollThread, swapped on reorder, cleared on reap.
    std::atomic<std::thread::id> m_MainThreadId{std::thread::id()};
    std::atomic<std::thread::id> m_PollThreadIds[2];
    std::condition_variable_any m_Cond;         // Signals main thread on Report 6 data or shutdown
    std::atomic<bool> m_bShutdown{false};       // Set to true to stop the threads

    // --- Main loop cadence ---
    std::atomic<int> m_iMainThreadSleepMs{50};  // Main thread sleep between iterations (ms)

    // Whether the input callback fires on every Report 3 packet (all-packets
    // mode) or only on change. Remembered here because a connection's shared
    // state is recreated on each Open, so this must be re-applied to every newly
    // connected pad (not just the ones connected when SetInputStateMode is called).
    std::atomic<bool> m_bAlwaysFireInput{false};

    // --- Devices and discovery ---
    SMXDevice m_Devices[2];                     // Pad slots: index 0 = P1, index 1 = P2
    std::string m_asPlayerAssignment[2];        // Serial pinned to each slot (empty = follow jumper)
    std::function<void(int, SMXUpdateCallbackReason)> m_Callback;  // Application update callback
    std::unique_ptr<IHIDEnumerator> m_pEnumerator;  // HID device enumerator (real or fake)
    double m_fLastEnumerationTime = 0;          // Rate-limits HID enumeration to 1/sec

    // --- Panel test mode ---
    PanelTestMode m_PanelTestMode = PanelTestMode_Off;       // Requested mode
    PanelTestMode m_LastSentPanelTestMode = PanelTestMode_Off; // Last mode sent to device
    double m_fLastPanelTestModeSentAt = 0;                   // For periodic refresh (~1s)

    // --- Lights command queue ---
    std::vector<PendingLightsCommand> m_aPendingLightsCommands;  // Scheduled lights commands
    double m_fDelayLightCommandsUntil = 0;  // Rate-limits lights to 30 FPS
};

} // namespace SMX

#endif
