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
        double fTimeToSend = 0;
        std::string sPadCommand[2];
    };

    std::recursive_mutex m_Lock;
    std::thread m_Thread;
    std::thread m_USBPollingThread;
    std::thread::id m_MainThreadId;
    std::thread::id m_USBPollingThreadId;
    std::condition_variable_any m_Cond;
    std::atomic<bool> m_bShutdown{false};
    std::atomic<int> m_iMainThreadSleepMs{50};
    std::atomic<int> m_iUSBPollingSleepUs{1000};
    SMXDevice m_Devices[2];
    std::function<void(int, SMXUpdateCallbackReason)> m_Callback;
    std::unique_ptr<IHIDEnumerator> m_pEnumerator;
    PanelTestMode m_PanelTestMode = PanelTestMode_Off;
    PanelTestMode m_LastSentPanelTestMode = PanelTestMode_Off;
    double m_fLastPanelTestModeSentAt = 0;
    double m_fLastEnumerationTime = 0;

    // Lights command queue and rate limiting state.
    std::vector<PendingLightsCommand> m_aPendingLightsCommands;
    double m_fDelayLightCommandsUntil = 0;
    int m_iLightsCommandsInProgress = 0;
};

} // namespace SMX

#endif
