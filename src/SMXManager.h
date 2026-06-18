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
// - Running a USB polling thread for low-latency input state updates
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

    void SetPollingRate(int iMainThreadMs, int iUSBPollingUs);
    void ReenableAutoLights();
    void SetPlatformLights(const char *pLightData);

    /// Sets panel LED colors for both pads. Data is split per-pad internally.
    void SetLights(const char *pLightData, int iLightDataSize);

    void SetPanelTestMode(PanelTestMode mode);
    void SetInputStateMode(bool bAlwaysFire);

private:
    void USBPollingThreadMain();
    void ThreadMain();
    void UpdatePanelTestMode();
    void AttemptConnections();
    bool CorrectDeviceOrder();
    void SendPendingLightsCommands();

    /// A single lights command to be sent to both pads at a scheduled time.
    struct PendingLightsCommand {
        double fTimeToSend = 0;       // Monotonic time when this command should be dispatched
        std::string sPadCommand[2];   // Command string per pad (empty = skip)
    };

    // --- Synchronization and threading ---
    std::recursive_mutex m_Lock;                // Protects all mutable state below
    // Guards the USB polling thread's access to the device read handles and the
    // m_Devices array shape, separate from m_Lock so a blocking write held under
    // m_Lock (CheckWrites) can never stall input reads. The USB polling thread
    // takes ONLY this lock; the main thread takes m_Lock and then m_PollLock
    // (lock order: m_Lock -> m_PollLock) only when it opens, closes, or swaps a
    // connection. The input callback fired from PollUSBData runs under this lock
    // and must not call back into m_Lock-taking SMX_* APIs (SMX_GetInputState is
    // lock-free and safe); see the threading notes in SMXDeviceConnection.h.
    std::mutex m_PollLock;
    std::thread m_Thread;                       // Main I/O thread (connections, commands, config)
    std::thread m_USBPollingThread;             // USB polling thread (input state reads)
    // For deadlock detection in destructor. Atomic because each is written by its
    // thread at startup and read by the destructor without other synchronization.
    std::atomic<std::thread::id> m_MainThreadId{std::thread::id()};
    std::atomic<std::thread::id> m_USBPollingThreadId{std::thread::id()};
    std::condition_variable_any m_Cond;         // Signals main thread on Report 6 data or shutdown
    std::atomic<bool> m_bShutdown{false};       // Set to true to stop both threads

    // --- Polling rate configuration ---
    std::atomic<int> m_iMainThreadSleepMs{50};  // Main thread sleep between iterations (ms)
    std::atomic<int> m_iUSBPollingSleepUs{1000}; // USB polling thread sleep between cycles (µs)

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
