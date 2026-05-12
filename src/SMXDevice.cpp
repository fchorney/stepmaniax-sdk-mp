#include "SMXDevice.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

#include "SMXConfigPacket.h"
#include "SMXHelpers.h"
#include "SMXProtocolConstants.h"

using namespace std;

namespace {

/// Wire format for per-panel sensor test data, extracted bit-by-bit from interleaved response.
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

} // anonymous namespace

namespace SMX {

SMXDevice::SMXDevice(SMXDevice &&other) noexcept:
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

SMXDevice &SMXDevice::operator=(SMXDevice &&other) noexcept
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

bool SMXDevice::IsConnected() const
{
    lock_guard<recursive_mutex> lock(*m_pLock);
    return IsConnectedLocked();
}

void SMXDevice::SetConnectionCallbacks()
{
    m_Connection.SetInputStateChangedCallback([this]() {
        CallUpdateCallback(static_cast<SMXUpdateCallbackReason>(SMXUpdateCallback_Updated | SMXUpdateCallback_InputState));
    });
}

void SMXDevice::CloseDevice()
{
    m_Connection.Close();
    m_bHaveConfig = false;
    CallUpdateCallback(static_cast<SMXUpdateCallbackReason>(SMXUpdateCallback_Updated | SMXUpdateCallback_Disconnected));
}

bool SMXDevice::PollUSBData()
{
    if(!m_Connection.IsConnected())
        return false;
    return m_Connection.PollUSBData();
}

void SMXDevice::SendCommand(const string &cmd, const function<void(string)>& pComplete)
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

void SMXDevice::GetInfo(SMXInfo &info) const
{
    lock_guard<recursive_mutex> lock(*m_pLock);
    GetInfoLocked(info);
}

void SMXDevice::GetInfoLocked(SMXInfo &info) const
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

bool SMXDevice::IsPlayer2Locked() const
{
    return IsConnectedLocked() && m_Connection.GetDeviceInfo().m_bP2;
}

void SMXDevice::FactoryReset()
{
    lock_guard<recursive_mutex> lock(*m_pLock);
    if(!m_Connection.IsConnected())
        return;

    const SMXDeviceInfo di = m_Connection.GetDeviceInfo();
    m_Connection.SendCommand("f\n");
    m_Connection.SendCommand(di.m_iFirmwareVersion >= 5 ? "G" : "g\n");
}

bool SMXDevice::GetConfig(SMXConfig &config) const
{
    lock_guard<recursive_mutex> lock(*m_pLock);
    if(!IsConnectedLocked())
        return false;
    config = m_bSendConfig ? m_WantedConfig : m_Config;
    return true;
}

void SMXDevice::SetConfig(const SMXConfig &config)
{
    lock_guard<recursive_mutex> lock(*m_pLock);
    if(!IsConnectedLocked())
        return;
    m_WantedConfig = config;
    m_bSendConfig = true;
}

void SMXDevice::SetSensorTestMode(SensorTestMode mode)
{
    lock_guard<recursive_mutex> lock(*m_pLock);
    m_SensorTestMode = mode;
}

bool SMXDevice::GetTestData(SMXSensorTestModeData &data) const
{
    lock_guard<recursive_mutex> lock(*m_pLock);
    if(!m_bHaveSensorTestData)
        return false;
    data = m_SensorTestData;
    return true;
}

void SMXDevice::ForceRecalibration()
{
    lock_guard<recursive_mutex> lock(*m_pLock);
    if(!m_Connection.IsConnected())
        return;

    m_Connection.SendCommand("C\n");
}

void SMXDevice::FireConnectedCallback(int pad) const
{
    if(!m_pUpdateCallback)
        return;
    m_pUpdateCallback(pad, static_cast<SMXUpdateCallbackReason>(
        SMXUpdateCallback_Updated | SMXUpdateCallback_ConfigUpdated | SMXUpdateCallback_Connected));
}

void SMXDevice::Update(string &sError)
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

// --- Private methods ---

bool SMXDevice::IsConnectedLocked() const
{
    return m_Connection.IsConnectedWithDeviceInfo() && m_bHaveConfig;
}

void SMXDevice::CheckActive()
{
    if(!m_Connection.IsConnectedWithDeviceInfo() || m_Connection.GetActive())
        return;

    m_Connection.SetActive(true);
    const SMXDeviceInfo di = m_Connection.GetDeviceInfo();
    m_Connection.SendCommand(di.m_iFirmwareVersion >= 5 ? "G" : "g\n");
}

void SMXDevice::HandlePackets()
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
            raw.resize(sizeof(OldSMXConfig), 0);  // Pad to full struct size for safe access
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

void SMXDevice::CallUpdateCallback(SMXUpdateCallbackReason const reason) const
{
    if(!m_pUpdateCallback)
        return;
    m_pUpdateCallback(m_iPadIndex, reason);
}

void SMXDevice::SendConfig()
{
    if(!m_Connection.IsConnected() || !m_bSendConfig || m_bSendingConfig)
        return;
    if(!m_bHaveConfig)
        return;

    // Rate limit: don't write more than once per CONFIG_WRITE_RATE_LIMIT_SECONDS.
    double fNow = GetMonotonicTime();
    if(m_fDelayConfigUpdatesUntil > fNow)
        return;
    m_fDelayConfigUpdatesUntil = fNow + CONFIG_WRITE_RATE_LIMIT_SECONDS;

    const SMXDeviceInfo di = m_Connection.GetDeviceInfo();

    string sData;
    if(di.m_iFirmwareVersion >= 5)
    {
        sData.reserve(2 + sizeof(SMXConfig));
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

void SMXDevice::UpdateSensorTestMode()
{
    if(m_SensorTestMode == SensorTestMode_Off)
        return;

    if(m_WaitingForSensorTestModeResponse != SensorTestMode_Off)
    {
        // Timeout if no response received.
        if(GetMonotonicTime() - m_fSentSensorTestModeRequestAt < SENSOR_TEST_TIMEOUT_SECONDS)
            return;
    }

    m_WaitingForSensorTestModeResponse = m_SensorTestMode;
    m_fSentSensorTestModeRequestAt = GetMonotonicTime();
    string sCmd = "y";
    sCmd.push_back(static_cast<char>(m_SensorTestMode));
    sCmd.push_back('\n');
    m_Connection.SendCommand(sCmd);
}

void SMXDevice::HandleSensorTestDataResponse(const string &buf)
{
    if(buf.size() < 3)
        return;

    uint8_t iSize = static_cast<uint8_t>(buf[2]);
    if(static_cast<int>(buf.size()) < iSize * 2 + 3)
        return;

    SensorTestMode iMode = static_cast<SensorTestMode>(buf[1]);

    // Early-return if we're not waiting for this response or mode doesn't match.
    if(m_WaitingForSensorTestModeResponse == SensorTestMode_Off)
        return;
    if(iMode != m_WaitingForSensorTestModeResponse)
        return;

    m_WaitingForSensorTestModeResponse = SensorTestMode_Off;

    if(iMode != m_SensorTestMode)
        return;

    // Parse interleaved uint16_t data.
    vector<uint16_t> data;
    data.reserve(iSize);
    for(int i = 3; i < iSize * 2 + 3; i += 2)
    {
        uint16_t iValue = static_cast<uint8_t>(buf[i]) |
                          (static_cast<uint8_t>(buf[i+1]) << 8);
        data.push_back(iValue);
    }

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

} // namespace SMX
