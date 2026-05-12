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

    void SetPollingRate(int iMainThreadMs, int iUSBPollingUs);
    void ReenableAutoLights();
    void SetPlatformLights(const char *pLightData);

    /// Sets panel LED colors for both pads.
    void SetLights(const std::string sPanelLights[2]);

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
    std::thread m_Thread;                       // Main I/O thread (connections, commands, config)
    std::thread m_USBPollingThread;             // USB polling thread (input state reads)
    std::thread::id m_MainThreadId;             // For deadlock detection in destructor
    std::thread::id m_USBPollingThreadId;       // For deadlock detection in destructor
    std::condition_variable_any m_Cond;         // Signals main thread on Report 6 data or shutdown
    std::atomic<bool> m_bShutdown{false};       // Set to true to stop both threads

    // --- Polling rate configuration ---
    std::atomic<int> m_iMainThreadSleepMs{50};  // Main thread sleep between iterations (ms)
    std::atomic<int> m_iUSBPollingSleepUs{1000}; // USB polling thread sleep between cycles (µs)

    // --- Devices and discovery ---
    SMXDevice m_Devices[2];                     // Pad slots: index 0 = P1, index 1 = P2
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
    int m_iLightsCommandsInProgress = 0;    // Outstanding async lights commands
};

} // namespace SMX

#endif
