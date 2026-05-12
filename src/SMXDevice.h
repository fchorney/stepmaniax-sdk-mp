#ifndef SMXDevice_h
#define SMXDevice_h

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "SMX.h"
#include "SMXDeviceConnection.h"

namespace SMX {

// ---------------------------------------------------------------------------
// SMXDevice — high-level per-controller logic
// ---------------------------------------------------------------------------
// Represents a single StepManiaX controller and manages its connection state,
// configuration, and communication. Each instance handles one physical device
// and maintains synchronized state that can be queried by the application.
//
// This class is non-copyable but movable (to support storage in arrays and
// transfer between threads). Access to mutable state is protected by a shared
// mutex to ensure thread-safe queries and updates from the I/O thread.

class SMXDevice
{
public:
    SMXDevice() = default;
    ~SMXDevice() = default;

    // Non-copyable
    SMXDevice(const SMXDevice &) = delete;
    SMXDevice &operator=(const SMXDevice &) = delete;

    // Movable
    SMXDevice(SMXDevice &&other) noexcept;
    SMXDevice &operator=(SMXDevice &&other) noexcept;

    /// Sets the recursive mutex used for synchronizing access to this device's state.
    void SetLock(std::recursive_mutex *pLock) { m_pLock = pLock; }

    /// Sets the slot index (0 or 1) for this device, used in callbacks.
    void SetPadIndex(int i) { m_iPadIndex = i; }

    /// Sets the callback function to be invoked when this device's state changes.
    void SetUpdateCallback(std::function<void(int, SMXUpdateCallbackReason)> cb) { m_pUpdateCallback = std::move(cb); }

    SMXDeviceConnection *GetConnection() { return &m_Connection; }

    /// Returns the HID path of this device.
    std::string GetDevicePath() const { return m_Connection.GetPath(); }

    bool IsConnected() const;

    bool OpenDevice(const std::string &sPath, std::unique_ptr<IHIDDevice> pDevice) { return m_Connection.Open(sPath, std::move(pDevice)); }

    void SetConnectionCallbacks();
    void CloseDevice();
    bool PollUSBData();

    /// Queues a command to be sent to this device asynchronously.
    void SendCommand(const std::string &cmd, const std::function<void(std::string)>& pComplete = nullptr);

    /// Retrieves the current device information.
    void GetInfo(SMXInfo &info) const;

    /// Internal version of GetInfo that assumes the lock is already held.
    void GetInfoLocked(SMXInfo &info) const;

    /// Returns whether the device's physical jumper is set to Player 2 mode.
    /// Lock must be held by the caller.
    bool IsPlayer2Locked() const;

    /// Returns the current input state (pressed panels) for this device.
    uint16_t GetInputState() const { return m_Connection.GetInputState(); }

    /// Resets the device to factory default configuration.
    void FactoryReset();

    /// Retrieves the current device configuration.
    bool GetConfig(SMXConfig &config) const;

    /// Queues a new configuration to be written to the device.
    void SetConfig(const SMXConfig &config);

    /// Sets the sensor test mode for this device.
    void SetSensorTestMode(SensorTestMode mode);

    /// Retrieves the most recent sensor test data.
    bool GetTestData(SMXSensorTestModeData &data) const;

    /// Triggers an immediate sensor recalibration on this device.
    void ForceRecalibration();

    /// Fires the Connected callback for this device using the given slot index.
    void FireConnectedCallback(int pad) const;

    /// Updates the device state, called from the I/O thread each frame.
    void Update(std::string &sError);

private:
    bool IsConnectedLocked() const;
    void CheckActive();
    void HandlePackets();
    void CallUpdateCallback(SMXUpdateCallbackReason reason) const;
    void SendConfig();
    void UpdateSensorTestMode();
    void HandleSensorTestDataResponse(const std::string &buf);

    // --- Synchronization ---
    std::recursive_mutex *m_pLock = nullptr;  // Manager's lock, used for all mutable state access

    // --- Identity and callbacks ---
    int m_iPadIndex = 0;  // Slot index (0 = P1, 1 = P2), used in callback notifications
    std::function<void(int, SMXUpdateCallbackReason)> m_pUpdateCallback;

    // --- Connection ---
    SMXDeviceConnection m_Connection;  // Low-level HID I/O for this device

    // --- Configuration state ---
    SMXConfig m_Config;                    // Last config read from device
    SMXConfig m_WantedConfig;              // Pending config to write (set by SetConfig)
    bool m_bHaveConfig = false;            // True once initial config has been read
    bool m_bSendConfig = false;            // True if m_WantedConfig needs to be sent
    bool m_bSendingConfig = false;         // True while a config write is in flight
    double m_fDelayConfigUpdatesUntil = 0; // Rate-limit: earliest time next write is allowed

    // --- Sensor test mode ---
    SensorTestMode m_SensorTestMode = SensorTestMode_Off;                    // Currently requested mode
    SensorTestMode m_WaitingForSensorTestModeResponse = SensorTestMode_Off;  // Mode of outstanding request
    double m_fSentSensorTestModeRequestAt = 0;                               // For timeout detection
    SMXSensorTestModeData m_SensorTestData{};                                // Most recent test data
    bool m_bHaveSensorTestData = false;                                      // True once data received
};

} // namespace SMX

#endif
