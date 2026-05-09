// SMX SDK Multi Platform — consolidated implementation.
// This file contains the core implementation of the StepManiaX SDK Multi Platform, consolidated
// into a single file for easier compilation and distribution. It includes:
// - Helper utility functions (logging, time, formatting, binary conversion)
// - SMXDevice: high-level per-controller logic and state management
// - SMXManager: device enumeration, background I/O thread, and orchestration
// - Public C API: initialization, shutdown, and device queries

#include "SMX.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "SMXConfigPacket.h"
#include "SMXDeviceConnection.h"
#include "SMXHIDInterface.h"

using namespace std;

#include "SMXVersion.h"

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------
// This section contains utility functions used throughout the SDK for logging,
// timing, string formatting, and binary data conversion.

namespace SMX {

static atomic<SMXLogCallback*> g_LogCallback{nullptr};

/// Returns the elapsed time in seconds since program start using a high-resolution
/// monotonic clock. Used for timing commands and logging timestamps.
/// @return Elapsed time in seconds as a double.
double GetMonotonicTime()
{
    static auto start = chrono::steady_clock::now();
    return chrono::duration<double>(chrono::steady_clock::now() - start).count();
}

/// Logs a message with a timestamp prefix. If a custom log callback is set,
/// it will be used; otherwise, logs to stdout with the current monotonic time.
/// @param s The message to log.
void Log(const string &s)
{
    auto cb = g_LogCallback.load(memory_order_acquire);
    if(cb)
        cb(s.c_str());
    else
        printf("%6.3f: %s\n", GetMonotonicTime(), s.c_str());
}

/// Sets a custom callback function to handle all log messages from the SDK.
/// Thread-safe: can be called from any thread at any time.
/// @param callback Function that receives log strings. Pass nullptr to disable.
void SetLogCallback(SMXLogCallback *callback)
{
    g_LogCallback.store(callback, memory_order_release);
}

/// Formatted string printing using printf-style arguments. Returns a std::string
/// instead of printing directly, useful for building log messages and debug output.
/// @param fmt Printf-style format string.
/// @param ... Variable arguments to format.
/// @return The formatted string.
string ssprintf(const char *fmt, ...)
{
    char buf[512];
    va_list va;
    va_start(va, fmt);
    const int n = vsnprintf(buf, sizeof(buf), fmt, va);
    va_end(va);
    if(n < 0) return string("Error formatting: ") + fmt;
    if(n < static_cast<int>(sizeof(buf)))
        return string(buf, n);

    string s(n, '\0');
    va_start(va, fmt);
    vsnprintf(&s[0], n + 1, fmt, va);
    va_end(va);
    return s;
}

/// Converts binary data to a hexadecimal string representation.
/// Each byte is converted to two hex digits (lowercase).
/// @param pData Pointer to the binary data.
/// @param iNumBytes Number of bytes to convert.
/// @return Hexadecimal string representation of the binary data.
string BinaryToHex(const void *pData, const int iNumBytes)
{
    static const char hex[] = "0123456789abcdef";
    const auto *p = static_cast<const unsigned char*>(pData);
    string s(iNumBytes * 2, '\0');
    for(int i = 0; i < iNumBytes; i++)
    {
        s[i*2]   = hex[p[i] >> 4];
        s[i*2+1] = hex[p[i] & 0x0F];
    }
    return s;
}

string BinaryToHex(const string &sString)
{
    return BinaryToHex(sString.data(), static_cast<int>(sString.size()));
}

/// Generates a random serial number (16 random bytes).
/// Used to assign unique identifiers to devices that don't have a serial number.
/// @param pOut Pointer to a 16-byte buffer to receive the generated serial.
static void GenerateSerial(uint8_t *pOut)
{
    random_device rd;
    mt19937 gen(rd());
    uniform_int_distribution<int> dist(0, 255);
    for(int i = 0; i < SERIAL_SIZE; i++)
        pOut[i] = static_cast<uint8_t>(dist(gen));
}

} // namespace SMX

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

namespace {

using namespace SMX;

class SMXDevice
{
public:
    SMXDevice() = default;
    ~SMXDevice() = default;

    // Non-copyable (prevents accidental duplication of device handles)
    SMXDevice(const SMXDevice &) = delete;
    SMXDevice &operator=(const SMXDevice &) = delete;

    // Movable (required for storage in arrays and manager reordering)
    SMXDevice(SMXDevice &&other) noexcept:
        m_pLock(other.m_pLock),
        m_iPadIndex(other.m_iPadIndex),
        m_pUpdateCallback(std::move(other.m_pUpdateCallback)),
        m_Connection(std::move(other.m_Connection)),
        m_Config(other.m_Config),
        m_WantedConfig(other.m_WantedConfig),
        m_bHaveConfig(other.m_bHaveConfig),
        m_bSendConfig(other.m_bSendConfig),
        m_bSendingConfig(other.m_bSendingConfig),
        m_fDelayConfigUpdatesUntil(other.m_fDelayConfigUpdatesUntil),
        m_SensorTestMode(other.m_SensorTestMode),
        m_WaitingForSensorTestModeResponse(other.m_WaitingForSensorTestModeResponse),
        m_fSentSensorTestModeRequestAt(other.m_fSentSensorTestModeRequestAt),
        m_SensorTestData(other.m_SensorTestData),
        m_bHaveSensorTestData(other.m_bHaveSensorTestData)
    {
        other.m_bHaveConfig = false;
        other.m_bSendConfig = false;
        other.m_bSendingConfig = false;
        other.m_SensorTestMode = SensorTestMode_Off;
        other.m_bHaveSensorTestData = false;
    }

    SMXDevice &operator=(SMXDevice &&other) noexcept
    {
        if(this != &other)
        {
            m_pLock = other.m_pLock;
            m_iPadIndex = other.m_iPadIndex;
            m_pUpdateCallback = std::move(other.m_pUpdateCallback);
            m_Connection = std::move(other.m_Connection);
            m_Config = other.m_Config;
            m_WantedConfig = other.m_WantedConfig;
            m_bHaveConfig = other.m_bHaveConfig;
            m_bSendConfig = other.m_bSendConfig;
            m_bSendingConfig = other.m_bSendingConfig;
            m_fDelayConfigUpdatesUntil = other.m_fDelayConfigUpdatesUntil;
            m_SensorTestMode = other.m_SensorTestMode;
            m_WaitingForSensorTestModeResponse = other.m_WaitingForSensorTestModeResponse;
            m_fSentSensorTestModeRequestAt = other.m_fSentSensorTestModeRequestAt;
            m_SensorTestData = other.m_SensorTestData;
            m_bHaveSensorTestData = other.m_bHaveSensorTestData;
            other.m_bHaveConfig = false;
            other.m_bSendConfig = false;
            other.m_bSendingConfig = false;
            other.m_SensorTestMode = SensorTestMode_Off;
            other.m_bHaveSensorTestData = false;
        }
        return *this;
    }

    /// Sets the recursive mutex used for synchronizing access to this device's state.
    /// Called during initialization to point to the manager's lock.
    /// @param pLock Pointer to the recursive mutex.
    void SetLock(recursive_mutex *pLock) { m_pLock = pLock; }

    /// Sets the slot index (0 or 1) for this device, used in callbacks.
    void SetPadIndex(int i) { m_iPadIndex = i; }

    /// Sets the callback function to be invoked when this device's state changes
    /// (e.g., connection, disconnection, input state updates).
    /// @param cb Callback function with signature (int pad, SMXUpdateCallbackReason reason).
    void SetUpdateCallback(function<void(int, SMXUpdateCallbackReason)> cb) { m_pUpdateCallback = std::move(cb); }

    SMXDeviceConnection *GetConnection() { return &m_Connection; }

    /// Returns the HID path of this device.
    string GetDevicePath() const { return m_Connection.GetPath(); }

    bool IsConnected() const
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        return IsConnectedLocked();
    }

    bool OpenDevice(const string &sPath, unique_ptr<IHIDDevice> pDevice) { return m_Connection.Open(sPath, std::move(pDevice)); }

    void SetConnectionCallbacks()
    {
        m_Connection.SetInputStateChangedCallback([this]() {
            CallUpdateCallback(static_cast<SMXUpdateCallbackReason>(SMXUpdateCallback_Updated | SMXUpdateCallback_InputState));
        });
    }

    void CloseDevice()
    {
        m_Connection.Close();
        m_bHaveConfig = false;
        CallUpdateCallback(static_cast<SMXUpdateCallbackReason>(SMXUpdateCallback_Updated | SMXUpdateCallback_Disconnected));
    }

    bool PollUSBData()
    {
        if(!m_Connection.IsConnected())
            return false;
        return m_Connection.PollUSBData();
    }

    /// Queues a command to be sent to this device asynchronously.
    /// The command is sent in the background I/O thread.
    /// @param cmd The command string to send.
    /// @param pComplete Optional callback invoked when the command response is received.
    void SendCommand(const string &cmd, const function<void(string)>& pComplete = nullptr)
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        if(!m_Connection.IsConnected())
        {
            if(pComplete)
                pComplete("");
            return;
        }
        m_Connection.SendCommand(cmd, pComplete);
    }

    /// Retrieves the current device information (connection status, player number, serial).
    /// This is thread-safe; it acquires the lock and calls GetInfoLocked.
    /// @param info [out] SMXInfo structure to be filled with device information.
    void GetInfo(SMXInfo &info) const
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        GetInfoLocked(info);
    }

    /// Internal version of GetInfo that assumes the lock is already held.
    /// Populates the SMXInfo structure with current device state.
    /// @param info [out] SMXInfo structure to be filled.
    void GetInfoLocked(SMXInfo &info) const
    {
        info = SMXInfo();
        info.m_bConnected = IsConnectedLocked();
        if(!info.m_bConnected)
            return;

        const SMXDeviceInfo di = m_Connection.GetDeviceInfo();
        info.m_bIsPlayer2 = di.m_bP2;
        memcpy(info.m_Serial, di.m_Serial, sizeof(info.m_Serial));
        info.m_iFirmwareVersion = di.m_iFirmwareVersion;

        // Check if a serial number has been assigned. An unassigned serial
        // will be all zeros or all 0xFF in the raw bytes, which shows up as
        // "00000000000000000000000000000000" or "ffffffffffffffffffffffffffffffff".
        info.m_bHasSerialNumber = false;
        for(int i = 0; i < 32; i++)
        {
            if(info.m_Serial[i] != '0' && info.m_Serial[i] != 'f')
            {
                info.m_bHasSerialNumber = true;
                break;
            }
        }
    }

    /// Returns whether the device's physical jumper is set to Player 2 mode.
    /// Lock must be held by the caller.
    /// Only meaningful if the device is fully connected.
    /// Used internally for device ordering logic.
    /// @return True if this device's P2 jumper is set.
    bool IsPlayer2Locked() const
    {
        return IsConnectedLocked() && m_Connection.GetDeviceInfo().m_bP2;
    }

    /// Returns the current input state (pressed panels) for this device.
    /// The input state is a 16-bit mask where each bit represents a panel.
    /// @return The input state bitmask.
    uint16_t GetInputState() const
    {
        // No lock needed: m_iInputState is std::atomic<uint16_t>.
        return m_Connection.GetInputState();
    }

    /// Resets the device to factory default configuration.
    /// Sends the factory reset command, then re-reads the config from the device.
    void FactoryReset()
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        if(!m_Connection.IsConnected())
            return;

        const SMXDeviceInfo di = m_Connection.GetDeviceInfo();
        m_Connection.SendCommand("f\n");
        m_Connection.SendCommand(di.m_iFirmwareVersion >= 5 ? "G" : "g\n");
    }

    /// Retrieves the current device configuration.
    /// If a SetConfig write is pending, returns the pending config (optimistic read).
    /// @param config [out] SMXConfig structure to be filled.
    /// @return True if config is available (device connected with config read).
    bool GetConfig(SMXConfig &config) const
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        if(!IsConnectedLocked())
            return false;
        config = m_bSendConfig ? m_WantedConfig : m_Config;
        return true;
    }

    /// Queues a new configuration to be written to the device.
    /// The write is rate-limited to once per second to prevent EEPROM wear.
    /// @param config The configuration to write.
    void SetConfig(const SMXConfig &config)
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        if(!IsConnectedLocked())
            return;
        m_WantedConfig = config;
        m_bSendConfig = true;
    }

    /// Sets the sensor test mode for this device.
    void SetSensorTestMode(SensorTestMode mode)
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        m_SensorTestMode = mode;
    }

    /// Retrieves the most recent sensor test data.
    /// @return True if data is available.
    bool GetTestData(SMXSensorTestModeData &data) const
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        if(!m_bHaveSensorTestData)
            return false;
        data = m_SensorTestData;
        return true;
    }

    /// Triggers an immediate sensor recalibration on this device.
    void ForceRecalibration()
    {
        lock_guard<recursive_mutex> lock(*m_pLock);
        if(!m_Connection.IsConnected())
            return;

        m_Connection.SendCommand("C\n");
    }

    /// Fires the Connected callback for this device using the given slot index.
    /// Called by the manager after device ordering is corrected.
    void FireConnectedCallback(int pad) const
    {
        if(!m_pUpdateCallback)
            return;
        m_pUpdateCallback(pad, static_cast<SMXUpdateCallbackReason>(
            SMXUpdateCallback_Updated | SMXUpdateCallback_ConfigUpdated | SMXUpdateCallback_Connected));
    }

    /// Updates the device state, called from the I/O thread each frame.
    /// Checks for input changes, processes received packets, and manages the
    /// connection lifecycle. Called with the manager's lock already held.
    /// @param sError [out] Error message if an update fails.
    void Update(string &sError)
    {
        if(!m_Connection.IsConnected())
            return;

        CheckActive();
        SendConfig();
        UpdateSensorTestMode();

        m_Connection.Update(sError);
        if(!sError.empty())
            return;

        HandlePackets();
    }

private:
    /// Checks if the device is fully connected (has valid connection, device info, and config).
    /// @return True if all required state has been initialized.
    bool IsConnectedLocked() const
    {
        return m_Connection.IsConnectedWithDeviceInfo() && m_bHaveConfig;
    }

    /// Activates the device after initial connection to begin receiving input updates.
    /// Sends an "activate" command to the device (format depends on firmware version).
    void CheckActive()
    {
        if(!m_Connection.IsConnectedWithDeviceInfo() || m_Connection.GetActive())
            return;

        m_Connection.SetActive(true);
        const SMXDeviceInfo di = m_Connection.GetDeviceInfo();
        m_Connection.SendCommand(di.m_iFirmwareVersion >= 5 ? "G" : "g\n");
    }

    /// Processes received packets from the device, extracting configuration data.
    /// Reads packets from the connection and updates the cached config when a
    /// complete config packet is received.
    void HandlePackets()
    {
        string buf;
        while(m_Connection.ReadPacket(buf))
        {
            if(buf.empty())
                continue;

            if(buf[0] == 'y')
            {
                HandleSensorTestDataResponse(buf);
                continue;
            }

            // We currently only handle g/G packets.
            if(buf[0] != 'g' && buf[0] != 'G')
                continue;

            if(buf.size() < 2)
            {
                Log("Invalid config packet");
                continue;
            }
            const auto iSize = static_cast<uint8_t>(buf[1]);
            if(static_cast<int>(buf.size()) < iSize + 2)
            {
                Log("Invalid config packet size");
                continue;
            }

            if(buf[0] == 'g')
            {
                vector<uint8_t> raw(buf.begin() + 2, buf.begin() + 2 + iSize);
                ConvertToNewConfig(raw, m_Config);
            }
            else
            {
                memcpy(&m_Config, buf.data() + 2, min(static_cast<int>(iSize), static_cast<int>(sizeof(m_Config))));
            }

            m_bHaveConfig = true;
            CallUpdateCallback(static_cast<SMXUpdateCallbackReason>(
                SMXUpdateCallback_Updated | SMXUpdateCallback_ConfigUpdated));
        }
    }

    /// Invokes the update callback if one is set, passing device index and reason.
    /// @param reason The reason for the callback (e.g., SMXUpdateCallback_Updated).
    void CallUpdateCallback(SMXUpdateCallbackReason const reason) const
    {
        if(!m_pUpdateCallback)
            return;
        m_pUpdateCallback(m_iPadIndex, reason);
    }

    /// Sends pending config to the device if conditions are met.
    /// Rate-limited to once per second to prevent EEPROM wear.
    void SendConfig()
    {
        if(!m_Connection.IsConnected() || !m_bSendConfig || m_bSendingConfig)
            return;
        if(!m_bHaveConfig)
            return;

        // Rate limit: don't write more than once per second.
        double fNow = GetMonotonicTime();
        if(m_fDelayConfigUpdatesUntil > fNow)
            return;
        m_fDelayConfigUpdatesUntil = fNow + 1.0;

        const SMXDeviceInfo di = m_Connection.GetDeviceInfo();

        string sData;
        if(di.m_iFirmwareVersion >= 5)
        {
            sData = "W";
            uint8_t iSize = sizeof(SMXConfig);
            sData.append(reinterpret_cast<char*>(&iSize), 1);
            sData.append(reinterpret_cast<const char*>(&m_WantedConfig), sizeof(SMXConfig));
        }
        else
        {
            sData = "w";
            vector<uint8_t> outputConfig(reinterpret_cast<const uint8_t*>(&m_Config),
                                         reinterpret_cast<const uint8_t*>(&m_Config) + sizeof(SMXConfig));
            ConvertToOldConfig(m_WantedConfig, outputConfig);
            uint8_t iSize = static_cast<uint8_t>(outputConfig.size());
            sData.append(reinterpret_cast<char*>(&iSize), 1);
            sData.append(reinterpret_cast<const char*>(outputConfig.data()), outputConfig.size());
        }

        m_bSendingConfig = true;
        m_Connection.SendCommand(sData, [this](string) {
            m_bSendingConfig = false;
        });
        m_bSendConfig = false;

        // Update cached config optimistically.
        m_Config = m_WantedConfig;

        // Read back to verify.
        m_Connection.SendCommand(di.m_iFirmwareVersion >= 5 ? "G" : "g\n");
    }

    /// Sends a sensor test mode request if one is active and no request is outstanding.
    void UpdateSensorTestMode()
    {
        if(m_SensorTestMode == SensorTestMode_Off)
            return;

        if(m_WaitingForSensorTestModeResponse != SensorTestMode_Off)
        {
            // Timeout after 2 seconds.
            if(GetMonotonicTime() - m_fSentSensorTestModeRequestAt < 2.0)
                return;
        }

        m_WaitingForSensorTestModeResponse = m_SensorTestMode;
        m_fSentSensorTestModeRequestAt = GetMonotonicTime();
        m_Connection.SendCommand(ssprintf("y%c\n", m_SensorTestMode));
    }

    /// Handles a 'y' sensor test data response packet.
    void HandleSensorTestDataResponse(const string &buf)
    {
        if(buf.size() < 3)
            return;

        uint8_t iSize = static_cast<uint8_t>(buf[2]);
        if(static_cast<int>(buf.size()) < iSize * 2 + 3)
            return;

        SensorTestMode iMode = static_cast<SensorTestMode>(buf[1]);

        // Parse interleaved uint16_t data.
        vector<uint16_t> data;
        for(int i = 3; i < iSize * 2 + 3; i += 2)
        {
            uint16_t iValue = static_cast<uint8_t>(buf[i]) |
                              (static_cast<uint8_t>(buf[i+1]) << 8);
            data.push_back(iValue);
        }

        if(m_WaitingForSensorTestModeResponse == SensorTestMode_Off)
            return;
        if(iMode != m_WaitingForSensorTestModeResponse)
            return;

        m_WaitingForSensorTestModeResponse = SensorTestMode_Off;

        if(iMode != m_SensorTestMode)
            return;

#pragma pack(push, 1)
        struct detail_data {
            uint8_t sig1:1, sig2:1, sig3:1;
            uint8_t bad_sensor_0:1, bad_sensor_1:1, bad_sensor_2:1, bad_sensor_3:1;
            uint8_t dummy:1;
            int16_t sensors[4];
            uint8_t dip:4;
            uint8_t bad_sensor_dip_0:1, bad_sensor_dip_1:1, bad_sensor_dip_2:1, bad_sensor_dip_3:1;
        };
#pragma pack(pop)

        SMXSensorTestModeData &output = m_SensorTestData;
        memset(&output, 0, sizeof(output));

        int iFwVersion = m_Connection.GetDeviceInfo().m_iFirmwareVersion;
        for(int iPanel = 0; iPanel < 9; iPanel++)
        {
            detail_data pad_data;
            // Extract bits for this panel from interleaved data.
            uint8_t *p = reinterpret_cast<uint8_t*>(&pad_data);
            for(int i = 0; i < static_cast<int>(sizeof(pad_data)); i++)
            {
                uint8_t result = 0;
                for(int j = 0; j < 8; j++)
                {
                    int iBit = i * 8 + j;
                    if(iBit < static_cast<int>(data.size()))
                        result |= ((data[iBit] >> iPanel) & 1) << j;
                }
                p[i] = result;
            }

            if(pad_data.sig1 != 0 || pad_data.sig2 != 1 || pad_data.sig3 != 0)
            {
                output.bHaveDataFromPanel[iPanel] = false;
                continue;
            }
            output.bHaveDataFromPanel[iPanel] = true;

            output.bBadSensorInput[iPanel][0] = pad_data.bad_sensor_0;
            output.bBadSensorInput[iPanel][1] = pad_data.bad_sensor_1;
            output.bBadSensorInput[iPanel][2] = pad_data.bad_sensor_2;
            output.bBadSensorInput[iPanel][3] = pad_data.bad_sensor_3;
            output.iDIPSwitchPerPanel[iPanel] = pad_data.dip;
            output.iBadJumper[iPanel][0] = pad_data.bad_sensor_dip_0;
            output.iBadJumper[iPanel][1] = pad_data.bad_sensor_dip_1;
            output.iBadJumper[iPanel][2] = pad_data.bad_sensor_dip_2;
            output.iBadJumper[iPanel][3] = pad_data.bad_sensor_dip_3;

            // Disable bad sensor flags for FSRs (activates spuriously).
            if(iFwVersion >= 5)
                for(int s = 0; s < 4; s++)
                    output.bBadSensorInput[iPanel][s] = false;

            for(int s = 0; s < 4; s++)
                output.sensorLevel[iPanel][s] = pad_data.sensors[s];
        }

        m_bHaveSensorTestData = true;
        CallUpdateCallback(static_cast<SMXUpdateCallbackReason>(SMXUpdateCallback_Updated | SMXUpdateCallback_SensorTestData));
    }

    recursive_mutex *m_pLock = nullptr;
    int m_iPadIndex = 0;
    function<void(int, SMXUpdateCallbackReason)> m_pUpdateCallback;
    SMXDeviceConnection m_Connection;
    SMXConfig m_Config;
    SMXConfig m_WantedConfig;
    bool m_bHaveConfig = false;
    bool m_bSendConfig = false;
    bool m_bSendingConfig = false;
    double m_fDelayConfigUpdatesUntil = 0;
    SensorTestMode m_SensorTestMode = SensorTestMode_Off;
    SensorTestMode m_WaitingForSensorTestModeResponse = SensorTestMode_Off;
    double m_fSentSensorTestModeRequestAt = 0;
    SMXSensorTestModeData m_SensorTestData{};
    bool m_bHaveSensorTestData = false;
};

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
    /// @param callback Function to be invoked when device state changes.
    explicit SMXManager(const function<void(int, SMXUpdateCallbackReason)>& callback):
        m_Callback(callback),
        m_pEnumerator(CreateHIDAPIEnumerator())
    {
        m_pEnumerator->Init();
        for(int i = 0; i < 2; i++)
        {
            m_Devices[i].SetLock(&m_Lock);
            m_Devices[i].SetPadIndex(i);
            m_Devices[i].SetUpdateCallback(callback);
            m_Devices[i].SetConnectionCallbacks();
        }
        m_Thread = thread([this] { ThreadMain(); });
        m_USBPollingThread = thread([this] { USBPollingThreadMain(); });
    }

    /// Constructor that accepts a custom enumerator (for testing).
    SMXManager(const function<void(int, SMXUpdateCallbackReason)>& callback, unique_ptr<IHIDEnumerator> pEnumerator):
        m_Callback(callback),
        m_pEnumerator(std::move(pEnumerator))
    {
        m_pEnumerator->Init();
        for(int i = 0; i < 2; i++)
        {
            m_Devices[i].SetLock(&m_Lock);
            m_Devices[i].SetPadIndex(i);
            m_Devices[i].SetUpdateCallback(callback);
            m_Devices[i].SetConnectionCallbacks();
        }
        m_Thread = thread([this] { ThreadMain(); });
        m_USBPollingThread = thread([this] { USBPollingThreadMain(); });
    }

    /// Destructor signals the I/O thread to stop and waits for it to finish.
    ~SMXManager()
    {
        m_bShutdown = true;
        m_Cond.notify_all();
        if(m_Thread.joinable())
        {
            m_Thread.join();
        }
        if(m_USBPollingThread.joinable())
        {
            m_USBPollingThread.join();
        }
        m_pEnumerator->Exit();
    }

    /// Retrieves a pointer to a device by pad index (0 or 1).
    /// @param pad Device index (0 for Player 1, 1 for Player 2).
    /// @return Pointer to the SMXDevice, or nullptr if pad is invalid.
    SMXDevice *GetDevice(const int pad)
    {
        if(pad < 0 || pad > 1)
            return nullptr;
        return &m_Devices[pad];
    }

    /// Generates and assigns random serial numbers to all connected devices
    /// that don't already have one. Called via the public API function SMX_SetSerialNumbers.
    void SetSerialNumbers()
    {
        lock_guard<recursive_mutex> lock(m_Lock);
        for(auto & m_Device : m_Devices)
        {
            string sData = "s";
            uint8_t serial[SERIAL_SIZE];
            GenerateSerial(serial);
            sData.append(reinterpret_cast<char*>(serial), sizeof(serial));
            sData.append(1, '\n');
            m_Device.SendCommand(sData);
        }
    }

    void SetPollingRate(int iMainThreadMs, int iUSBPollingUs)
    {
        m_iMainThreadSleepMs.store(iMainThreadMs);
        m_iUSBPollingSleepUs.store(iUSBPollingUs);
    }

    void ReenableAutoLights()
    {
        lock_guard<recursive_mutex> lock(m_Lock);
        for(auto & m_Device : m_Devices)
            m_Device.SendCommand(string("S 1\n", 4));
    }

    void SetPlatformLights(const char *pLightData)
    {
        lock_guard<recursive_mutex> lock(m_Lock);
        for(int iPad = 0; iPad < 2; iPad++)
        {
            if(!m_Devices[iPad].IsConnected())
                continue;

            SMXConfig config;
            if(!m_Devices[iPad].GetConfig(config))
                continue;
            if(config.masterVersion < 4)
                continue;

            string sCmd;
            sCmd.push_back('L');
            sCmd.push_back(0);   // strip index
            sCmd.push_back(44);  // number of LEDs
            sCmd.append(pLightData + iPad * 44 * 3, 44 * 3);
            m_Devices[iPad].SendCommand(sCmd);
        }
    }

    void SetPanelTestMode(PanelTestMode mode)
    {
        lock_guard<recursive_mutex> lock(m_Lock);
        m_PanelTestMode = mode;
        m_Cond.notify_all();
    }

    void SetInputStateMode(bool bAlwaysFire)
    {
        for(auto & m_Device : m_Devices)
            m_Device.GetConnection()->SetAlwaysFireInputCallback(bAlwaysFire);
    }

private:
    /// Main loop for the USB polling thread. Runs continuously, checking both devices
    /// for available USB data and signaling the main I/O thread when data is found.
    ///
    /// The USB polling thread:
    /// - Continuously reads from both HID devices (non-blocking)
    /// - Parses Report 3 (input state) packets inline and updates m_iInputState atomically
    /// - Buffers Report 6 (command/config) packets for main thread processing
    /// - Signals the main thread if Report 6 packets are found
    /// - Sleep interval is configurable via SMX_SetPollingRate (default: 1000us)
    void USBPollingThreadMain()
    {
        while(!m_bShutdown)
        {
            bool bHasReport6Data = false;

            {
                lock_guard<recursive_mutex> lock(m_Lock);

                // Check both devices for available USB data
                for(int i = 0; i < 2; i++)
                {
                    if(m_Devices[i].PollUSBData() || m_Devices[i].GetConnection()->HasReadError())
                        bHasReport6Data = true;
                }
            }

            // Signal the main thread if we found any Report 6 data to process
            if(bHasReport6Data)
            {
                m_Cond.notify_all();
            }

            // Configurable sleep between poll cycles
            this_thread::sleep_for(chrono::microseconds(m_iUSBPollingSleepUs.load(memory_order_relaxed)));
        }
    }
    /// Main loop for the background I/O thread. Runs continuously until shutdown.
    /// Each iteration:
    /// - Attempts to connect to any newly discovered devices
    /// - Updates each connected device's state
    /// - Ensures devices are in the correct order (Player 1, then Player 2)
    /// - Waits for Report 6 data or timeout before the next iteration
    void ThreadMain()
    {
        m_Lock.lock();
        while(!m_bShutdown)
        {
            AttemptConnections();

            bool bWasConnected[2] = {m_Devices[0].IsConnected(), m_Devices[1].IsConnected()};

            for(int i = 0; i < 2; i++)
            {
                string sError;
                m_Devices[i].Update(sError);
                if(!sError.empty())
                {
                    Log(ssprintf("Device %i error: %s", i, sError.c_str()));
                    m_Devices[i].CloseDevice();
                }
            }

            // Correct device ordering BEFORE firing Connected callbacks.
            // This ensures SMX_GetInfo(pad) returns the correct device when
            // the callback handler queries it.
            const bool bSwapped = CorrectDeviceOrder();

            // Detect which slots just transitioned to connected, accounting for swap.
            bool bJustConnected[2] = {
                !bWasConnected[0] && m_Devices[0].IsConnected(),
                !bWasConnected[1] && m_Devices[1].IsConnected()
            };
            if(bSwapped)
            {
                // After a swap, slot i now holds what was in slot 1-i before.
                // A device that "just connected" in the old slot is now in the new slot.
                bJustConnected[0] = !bWasConnected[1] && m_Devices[0].IsConnected();
                bJustConnected[1] = !bWasConnected[0] && m_Devices[1].IsConnected();
            }

            for(int i = 0; i < 2; i++)
            {
                if(bJustConnected[i])
                    m_Devices[i].FireConnectedCallback(i);
            }

            UpdatePanelTestMode();

            // Wait for Report 6 data from USB polling thread, or timeout.
            m_Cond.wait_for(m_Lock, chrono::milliseconds(m_iMainThreadSleepMs.load(memory_order_relaxed)));
        }
        m_Lock.unlock();
    }

    /// Periodically resends the panel test mode command to keep it active.
    /// The device times out after ~1 second without a refresh.
    void UpdatePanelTestMode()
    {
        if(m_PanelTestMode == m_LastSentPanelTestMode &&
           (m_PanelTestMode == PanelTestMode_Off || GetMonotonicTime() - m_fLastPanelTestModeSentAt < 1.0))
            return;

        // TODO: When transitioning from Off to active (m_LastSentPanelTestMode == PanelTestMode_Off),
        // send a lights-off command ("l" + 108 zero bytes + "\n") to clear panels before entering
        // test mode. This matches the original SDK behavior. Requires lighting commands to be implemented first.

        m_fLastPanelTestModeSentAt = GetMonotonicTime();
        m_LastSentPanelTestMode = m_PanelTestMode;
        for(auto & m_Device : m_Devices)
            m_Device.SendCommand(ssprintf("t %c\n", m_PanelTestMode));
    }

    /// Enumerates all HID devices matching the SMX vendor ID and product ID.
    /// For each discovered device not already connected, attempts to open it
    /// in an available slot. Called each I/O thread iteration.
    void AttemptConnections()
    {
        // Skip enumeration if both device slots are already occupied.
        if(!m_Devices[0].GetDevicePath().empty() && !m_Devices[1].GetDevicePath().empty())
            return;

        // Rate-limit enumeration to once per second to reduce syscalls.
        double fNow = GetMonotonicTime();
        if(fNow - m_fLastEnumerationTime < 1.0)
            return;
        m_fLastEnumerationTime = fNow;

        // Enumerate SMX devices via the HID enumerator.
        auto devs = m_pEnumerator->Enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
        for(const auto &dev : devs)
        {
            if(dev.sProduct != SMX_USB_PRODUCT_STRING)
                continue;
            if(dev.sPath.empty())
                continue;

            // Skip if already open.
            bool bOpen = false;
            for(const auto & m_Device : m_Devices)
                if(m_Device.GetDevicePath() == dev.sPath) { bOpen = true; break; }
            if(bOpen) continue;

            // Find an empty slot.
            SMXDevice *pSlot = nullptr;
            for(auto & m_Device : m_Devices)
                if(m_Device.GetDevicePath().empty()) { pSlot = &m_Device; break; }

            if(!pSlot) { Log("No available slots for device."); break; }

            Log("Opening SMX device: " + dev.sPath);
            auto pDevice = m_pEnumerator->Open(dev.sPath);
            if(!pDevice)
            {
                Log("Error opening device: " + dev.sPath);
                continue;
            }
            pSlot->OpenDevice(dev.sPath, std::move(pDevice));
        }
    }

    /// Ensures devices are in the correct order, swapping them if necessary.
    /// The correct order is:
    /// - Slot 0: Player 1 device (if connected)
    /// - Slot 1: Player 2 device (if connected)
    ///
    /// This function is called each I/O thread iteration to maintain proper
    /// device ordering as devices are connected and disconnected.
    /// @return true if devices were swapped.
    bool CorrectDeviceOrder()
    {
        SMXInfo info[2];
        m_Devices[0].GetInfoLocked(info[0]);
        m_Devices[1].GetInfoLocked(info[1]);

        if(info[0].m_bConnected && info[1].m_bConnected &&
           m_Devices[0].IsPlayer2Locked() == m_Devices[1].IsPlayer2Locked())
            return false;

        const bool bSwap = (info[0].m_bConnected && m_Devices[0].IsPlayer2Locked()) ||
                     (info[1].m_bConnected && !m_Devices[1].IsPlayer2Locked());
        if(bSwap)
        {
            SMXDevice temp(std::move(m_Devices[0]));
            m_Devices[0] = std::move(m_Devices[1]);
            m_Devices[1] = std::move(temp);

            // Re-bind callbacks and pad indices after swap since the devices
            // are now in different slots.
            m_Devices[0].SetConnectionCallbacks();
            m_Devices[1].SetConnectionCallbacks();
            m_Devices[0].SetPadIndex(0);
            m_Devices[1].SetPadIndex(1);
        }
        return bSwap;
    }

    recursive_mutex m_Lock;
    thread m_Thread;
    thread m_USBPollingThread;
    condition_variable_any m_Cond;
    atomic<bool> m_bShutdown{false};
    atomic<int> m_iMainThreadSleepMs{50};
    atomic<int> m_iUSBPollingSleepUs{1000};
    SMXDevice m_Devices[2];
    function<void(int, SMXUpdateCallbackReason)> m_Callback;
    unique_ptr<IHIDEnumerator> m_pEnumerator;
    PanelTestMode m_PanelTestMode = PanelTestMode_Off;
    PanelTestMode m_LastSentPanelTestMode = PanelTestMode_Off;
    double m_fLastPanelTestModeSentAt = 0;
    double m_fLastEnumerationTime = 0;
};

// File-static singleton. No global variable visible outside this file.
shared_ptr<SMXManager> g_pSMX;

} // anonymous namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
// This section contains the C API functions exported to applications using the SDK.

/// Initializes the SMX SDK and starts searching for connected devices.
/// Must be called once before using any other SDK functions.
/// The background I/O thread will automatically discover connected devices and
/// invoke the update callback when their state changes.
///
/// @warning The callback may be invoked from different background threads (the USB
/// polling thread for input state changes, and the main I/O thread for connection
/// and config events). Invocations are serialized internally so the callback will
/// never be called from two threads simultaneously, but it will not necessarily be
/// called from the application's main thread.
///
/// @param callback Function to be called asynchronously when devices are connected,
///                  disconnected, or their input state changes.
/// @param pUser Application-defined pointer passed to all callbacks for context.
SMX_API void SMX_Start(SMXUpdateCallback callback, void *pUser)
{
    if(g_pSMX) return;

    auto cb = [callback, pUser](const int pad, const SMXUpdateCallbackReason reason) {
        callback(pad, reason, pUser);
    };
    g_pSMX = make_shared<SMXManager>(cb);
}

/// Shuts down the SMX SDK and disconnects from all devices.
/// Stops the background I/O thread and cleans up resources.
/// IMPORTANT: This must not be called from within an update callback.
SMX_API void SMX_Stop()
{
    g_pSMX.reset();
}

/// Sets a custom callback function to receive diagnostic log messages.
/// If not set, log messages are printed to stdout with timestamps.
/// This can be called before SMX_Start to capture initialization logs.
/// @param callback Function that receives log strings. Can be nullptr to disable.
SMX_API void SMX_SetLogCallback(SMXLogCallback callback)
{
    SetLogCallback(callback);
}

/// Queries information about a connected device.
/// Use this to detect which pads are connected and retrieve their properties
/// (serial number, firmware version, player number).
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param info [out] Pointer to SMXInfo structure to be filled with device info.
SMX_API void SMX_GetInfo(const int pad, SMXInfo *info)
{
    if(!g_pSMX) return;
    const auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->GetInfo(*info);
}

/// Retrieves the current configuration for a device.
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param config [out] Pointer to SMXConfig structure to be filled.
/// @return True if config was retrieved, false if device is not connected.
SMX_API bool SMX_GetConfig(const int pad, SMXConfig *config)
{
    if(!g_pSMX || !config) return false;
    const auto *dev = g_pSMX->GetDevice(pad);
    if(!dev) return false;
    return dev->GetConfig(*config);
}

/// Writes a new configuration to a device.
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @param config Pointer to the SMXConfig to write.
SMX_API void SMX_SetConfig(const int pad, const SMXConfig *config)
{
    if(!g_pSMX || !config) return;
    auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->SetConfig(*config);
}

/// Retrieves the current input state (pressed panels) for a device.
/// The returned value is a 16-bit bitmask where each bit corresponds to a panel.
/// @param pad Device index (0 for Player 1, 1 for Player 2).
/// @return 16-bit input state bitmask. Returns 0 if the device is not connected.
SMX_API uint16_t SMX_GetInputState(const int pad)
{
    if(!g_pSMX) return 0;
    const auto *dev = g_pSMX->GetDevice(pad);
    return dev ? dev->GetInputState() : 0;
}

/// Assigns random serial numbers to all connected devices that don't have one.
/// The serial numbers are permanently written to the device's non-volatile memory.
/// This is an asynchronous operation; the actual programming happens in the
/// background I/O thread.
SMX_API void SMX_SetSerialNumbers()
{
    if(g_pSMX) g_pSMX->SetSerialNumbers();
}

/// Resets a pad to its factory default configuration.
/// The operation is asynchronous; the ConfigUpdated callback will fire when complete.
/// @param pad Device index (0 for Player 1, 1 for Player 2).
SMX_API void SMX_FactoryReset(const int pad)
{
    if(!g_pSMX) return;
    auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->FactoryReset();
}

/// Triggers an immediate sensor recalibration on the specified pad.
/// @param pad Device index (0 for Player 1, 1 for Player 2).
SMX_API void SMX_ForceRecalibration(const int pad)
{
    if(!g_pSMX) return;
    auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->ForceRecalibration();
}

/// Re-enables automatic panel lighting on both pads.
SMX_API void SMX_ReenableAutoLights()
{
    if(g_pSMX) g_pSMX->ReenableAutoLights();
}

/// Sets the platform edge LED strip colors for both pads.
SMX_API void SMX_SetPlatformLights(const char *pLightData)
{
    if(!g_pSMX || !pLightData) return;
    g_pSMX->SetPlatformLights(pLightData);
}

/// Sets a panel test mode on all connected pads.
/// The SDK periodically resends the command to keep the mode active.
SMX_API void SMX_SetPanelTestMode(PanelTestMode mode)
{
    if(g_pSMX) g_pSMX->SetPanelTestMode(mode);
}

/// Sets the sensor test mode for a pad.
SMX_API void SMX_SetTestMode(const int pad, SensorTestMode mode)
{
    if(!g_pSMX) return;
    auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->SetSensorTestMode(mode);
}

/// Retrieves the most recent sensor test data for a pad.
SMX_API bool SMX_GetTestData(const int pad, SMXSensorTestModeData *data)
{
    if(!g_pSMX || !data) return false;
    const auto *dev = g_pSMX->GetDevice(pad);
    if(!dev) return false;
    return dev->GetTestData(*data);
}

SMX_API void SMX_SetPollingRate(int iMainThreadMs, int iUSBPollingUs)
{
    if(iMainThreadMs > 100)
        Log(ssprintf("Warning: main thread sleep of %dms may delay device connections and cause missed serial numbers. Recommended: 50ms or below.", iMainThreadMs));
    if(g_pSMX) g_pSMX->SetPollingRate(iMainThreadMs, iUSBPollingUs);
}

SMX_API void SMX_SetInputStateMode(bool bAlwaysFire)
{
    if(g_pSMX) g_pSMX->SetInputStateMode(bAlwaysFire);
}

/// Returns the SDK version string.
/// @return C-string containing the version (e.g., "0.2.0").
SMX_API const char *SMX_Version()
{
    return SMX_VERSION;
}

/// Returns the elapsed time in seconds since the SDK was initialized.
/// This is useful for logging timestamps and measuring elapsed time.
/// @return Elapsed time in seconds as a double.
SMX_API double SMX_GetMonotonicTime()
{
    return GetMonotonicTime();
}

// ---------------------------------------------------------------------------
// Test-only API (not exported from shared library, linked directly in tests)
// ---------------------------------------------------------------------------

/// Starts the SDK with a custom HID enumerator for testing.
/// This allows tests to inject fake devices without real hardware.
void SMX_StartWithEnumerator(SMXUpdateCallback callback, void *pUser, std::unique_ptr<SMX::IHIDEnumerator> pEnumerator)
{
    if(g_pSMX) return;

    auto cb = [callback, pUser](const int pad, const SMXUpdateCallbackReason reason) {
        callback(pad, reason, pUser);
    };
    g_pSMX = make_shared<SMXManager>(cb, std::move(pEnumerator));
}

