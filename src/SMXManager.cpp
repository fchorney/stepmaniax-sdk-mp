#include "SMXManager.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>

#include "SMXDeviceConnection.h"
#include "SMXHelpers.h"
#include "SMXProtocolConstants.h"

using namespace std;

namespace SMX {

SMXManager::SMXManager(const function<void(int, SMXUpdateCallbackReason)>& callback):
    SMXManager(callback, CreateHIDAPIEnumerator())
{
}

SMXManager::SMXManager(const function<void(int, SMXUpdateCallbackReason)>& callback, unique_ptr<IHIDEnumerator> pEnumerator):
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

SMXManager::~SMXManager()
{
    // Detect if SMX_Stop() is being called from within a callback (which would deadlock).
    auto thisId = this_thread::get_id();
    if(thisId == m_MainThreadId || thisId == m_USBPollingThreadId)
    {
        Log(ssprintf("SMX_Stop() called from within an SDK callback — this will deadlock. Aborting. "
                     "(caller=%s, main=%s, usb=%s)",
                     thisId == m_MainThreadId ? "MainThread" : "USBThread",
                     m_MainThreadId == thread::id() ? "unset" : "set",
                     m_USBPollingThreadId == thread::id() ? "unset" : "set"));
        abort();
    }

    m_bShutdown = true;
    m_Cond.notify_all();
    if(m_Thread.joinable())
        m_Thread.join();
    if(m_USBPollingThread.joinable())
        m_USBPollingThread.join();
    m_pEnumerator->Exit();
}

SMXDevice *SMXManager::GetDevice(const int pad)
{
    if(pad < 0 || pad > 1)
        return nullptr;
    return &m_Devices[pad];
}

void SMXManager::SetSerialNumbers()
{
    lock_guard<recursive_mutex> lock(m_Lock);
    for(auto &device : m_Devices)
    {
        string sData = "s";
        uint8_t serial[SERIAL_SIZE];
        GenerateSerial(serial);
        sData.append(reinterpret_cast<char*>(serial), sizeof(serial));
        sData.append(1, '\n');
        device.SendCommand(sData);
    }
}

void SMXManager::SetPollingRate(int iMainThreadMs, int iUSBPollingUs)
{
    m_iMainThreadSleepMs.store(iMainThreadMs);
    m_iUSBPollingSleepUs.store(iUSBPollingUs);
}

void SMXManager::ReenableAutoLights()
{
    lock_guard<recursive_mutex> lock(m_Lock);
    for(auto &device : m_Devices)
        device.SendCommand("S 1\n");
}

void SMXManager::SetPlatformLights(const char *pLightData)
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
        sCmd.push_back(PLATFORM_STRIP_LEDS);
        sCmd.append(pLightData + iPad * PLATFORM_STRIP_LEDS * 3, PLATFORM_STRIP_LEDS * 3);
        m_Devices[iPad].SendCommand(sCmd);
    }
}

// Precomputed color scaling lookup table (avoids float multiplication on hot path).
namespace {
struct ColorScaleTable {
    uint8_t v[256];
    ColorScaleTable() {
        for(int i = 0; i < 256; i++)
            v[i] = static_cast<uint8_t>(i * LED_COLOR_SCALE);
    }
};
static const ColorScaleTable g_ColorScale;
} // anonymous namespace

void SMXManager::SetLights(const char *pLightData, int iLightDataSize)
{
    lock_guard<recursive_mutex> lock(m_Lock);

    // Don't send lights when a panel test mode is active.
    if(m_PanelTestMode != PanelTestMode_Off)
        return;

    // Determine per-pad size and validate.
    int iBytesPerPad;
    if(iLightDataSize == 2 * BYTES_PER_PAD_16)
        iBytesPerPad = BYTES_PER_PAD_16;
    else if(iLightDataSize == 2 * BYTES_PER_PAD_25)
        iBytesPerPad = BYTES_PER_PAD_25;
    else
        return;

    // Build the 3 commands per pad: '4' (inner 3x3), '2' (top half), '3' (bottom half).
    string sLightCommands[3][2]; // [command_index][pad]

    for(int iPad = 0; iPad < 2; ++iPad)
    {
        const char *pPadData = pLightData + iPad * iBytesPerPad;

        // Reserve known final sizes to avoid reallocations.
        // Command '4': 1 prefix + 9 panels × 9 LEDs × 3 RGB + 1 newline = 244
        // Command '2': 1 prefix + 9 panels × 8 LEDs × 3 RGB + 1 newline = 218
        // Command '3': same as '2' = 218
        sLightCommands[0][iPad].reserve(1 + NUM_PANELS * 9 * 3 + 1);
        sLightCommands[1][iPad].reserve(1 + NUM_PANELS * 8 * 3 + 1);
        sLightCommands[2][iPad].reserve(1 + NUM_PANELS * 8 * 3 + 1);

        sLightCommands[0][iPad] = "4";
        sLightCommands[1][iPad] = "2";
        sLightCommands[2][iPad] = "3";

        int iNextInputByte = 0;
        for(int iPanel = 0; iPanel < NUM_PANELS; ++iPanel)
        {
            // Outer 4x4 grid: top 2 rows → command '2', bottom 2 rows → command '3'.
            for(int iByte = 0; iByte < 4*4*3; ++iByte)
            {
                uint8_t iColor = g_ColorScale.v[static_cast<uint8_t>(pPadData[iNextInputByte++])];
                int iCmd = iByte < 4*2*3 ? 1 : 2;
                sLightCommands[iCmd][iPad].push_back(iColor);
            }
            // Inner 3x3 grid → command '4'.
            if(iBytesPerPad == BYTES_PER_PAD_25)
            {
                for(int iByte = 0; iByte < 3*3*3; ++iByte)
                {
                    uint8_t iColor = g_ColorScale.v[static_cast<uint8_t>(pPadData[iNextInputByte++])];
                    sLightCommands[0][iPad].push_back(iColor);
                }
            }
            else
            {
                // 16-LED mode: no inner grid data, fill with zeros.
                sLightCommands[0][iPad].append(3*3*3, '\0');
            }
        }

        sLightCommands[0][iPad].push_back('\n');
        sLightCommands[1][iPad].push_back('\n');
        sLightCommands[2][iPad].push_back('\n');
    }

    // Rate limiting: if we already have a full set of 3 pending commands,
    // replace them with the new data rather than adding more.
    if(m_aPendingLightsCommands.size() < 3)
    {
        double fNow = GetMonotonicTime();
        double fSendCommandAt = max(fNow, m_fDelayLightCommandsUntil);
        double fCommandTimes[3] = { fNow, fNow, fNow };

        bool bMasterIsV4 = false;
        bool bAnyConnected = false;
        for(int iPad = 0; iPad < 2; ++iPad)
        {
            SMXConfig config;
            if(!m_Devices[iPad].GetConfig(config))
                continue;
            bAnyConnected = true;
            if(config.masterVersion >= 4)
                bMasterIsV4 = true;
        }

        // Don't queue lights if no device has config yet.
        if(!bAnyConnected)
            return;

        // Firmware < 4: delay between commands to give the master time to relay.
        // Firmware >= 4: queue all at once, firmware handles flow control.
        if(!bMasterIsV4)
        {
            fCommandTimes[1] = fSendCommandAt;
            fCommandTimes[2] = fCommandTimes[1] + LIGHTS_LEGACY_COMMAND_DELAY;
        }

        m_fDelayLightCommandsUntil = fSendCommandAt + LIGHTS_FRAME_INTERVAL;

        m_aPendingLightsCommands.push_back(PendingLightsCommand{fCommandTimes[0], {"", ""}});
        m_aPendingLightsCommands.push_back(PendingLightsCommand{fCommandTimes[1], {"", ""}});
        m_aPendingLightsCommands.push_back(PendingLightsCommand{fCommandTimes[2], {"", ""}});
    }

    // Fill in (or replace) the last 3 pending commands with the new data.
    for(int iPad = 0; iPad < 2; ++iPad)
    {
        if(sLightCommands[0][iPad].empty())
            continue;

        SMXConfig config;
        if(!m_Devices[iPad].GetConfig(config))
            continue;

        size_t iBase = m_aPendingLightsCommands.size() - 3;

        // Command '4' (inner grid) is only sent on firmware v4+.
        if(config.masterVersion >= 4)
            m_aPendingLightsCommands[iBase].sPadCommand[iPad] = sLightCommands[0][iPad];
        else
            m_aPendingLightsCommands[iBase].sPadCommand[iPad] = "";

        m_aPendingLightsCommands[iBase + 1].sPadCommand[iPad] = sLightCommands[1][iPad];
        m_aPendingLightsCommands[iBase + 2].sPadCommand[iPad] = sLightCommands[2][iPad];
    }

    // Wake the main thread to send the commands.
    m_Cond.notify_all();
}

void SMXManager::SendPendingLightsCommands()
{
    while(!m_aPendingLightsCommands.empty())
    {
        const PendingLightsCommand &cmd = m_aPendingLightsCommands[0];
        if(cmd.fTimeToSend > GetMonotonicTime())
            break;

        for(int iPad = 0; iPad < 2; ++iPad)
        {
            if(!cmd.sPadCommand[iPad].empty())
            {
                m_iLightsCommandsInProgress++;
                m_Devices[iPad].SendCommand(cmd.sPadCommand[iPad], [this](string) {
                    lock_guard<recursive_mutex> lock(m_Lock);
                    m_iLightsCommandsInProgress--;
                });
            }
        }

        m_aPendingLightsCommands.erase(m_aPendingLightsCommands.begin());
    }
}

void SMXManager::SetPanelTestMode(PanelTestMode mode)
{
    lock_guard<recursive_mutex> lock(m_Lock);
    m_PanelTestMode = mode;
    m_Cond.notify_all();
}

void SMXManager::SetInputStateMode(bool bAlwaysFire)
{
    for(auto &device : m_Devices)
        device.GetConnection()->SetAlwaysFireInputCallback(bAlwaysFire);
}

// --- Private thread methods ---

void SMXManager::USBPollingThreadMain()
{
    m_USBPollingThreadId = this_thread::get_id();
    while(!m_bShutdown)
    {
        bool bHasReport6Data = false;

        {
            lock_guard<recursive_mutex> lock(m_Lock);
            for(int i = 0; i < 2; i++)
            {
                if(m_Devices[i].PollUSBData() || m_Devices[i].GetConnection()->HasReadError())
                    bHasReport6Data = true;
            }
        }

        if(bHasReport6Data)
            m_Cond.notify_all();

        this_thread::sleep_for(chrono::microseconds(m_iUSBPollingSleepUs.load(memory_order_relaxed)));
    }
}

void SMXManager::ThreadMain()
{
    m_MainThreadId = this_thread::get_id();
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
        const bool bDeviceJustConnected =
            (!bWasConnected[0] && m_Devices[0].IsConnected()) ||
            (!bWasConnected[1] && m_Devices[1].IsConnected());
        const bool bSwapped = bDeviceJustConnected && CorrectDeviceOrder();

        // Detect which slots just transitioned to connected, accounting for swap.
        bool bJustConnected[2] = {
            !bWasConnected[0] && m_Devices[0].IsConnected(),
            !bWasConnected[1] && m_Devices[1].IsConnected()
        };
        if(bSwapped)
        {
            bJustConnected[0] = !bWasConnected[1] && m_Devices[0].IsConnected();
            bJustConnected[1] = !bWasConnected[0] && m_Devices[1].IsConnected();
        }

        for(int i = 0; i < 2; i++)
        {
            if(bJustConnected[i])
                m_Devices[i].FireConnectedCallback(i);
        }

        UpdatePanelTestMode();
        SendPendingLightsCommands();

        // Determine wait time: if lights commands are pending, wake up when the
        // next one is due. Otherwise use the normal main thread sleep interval.
        int iWaitMs = m_iMainThreadSleepMs.load(memory_order_relaxed);
        if(!m_aPendingLightsCommands.empty())
        {
            double fSendIn = m_aPendingLightsCommands[0].fTimeToSend - GetMonotonicTime();
            int iLightsMs = int(fSendIn * 1000) + 1;
            if(iLightsMs < 0) iLightsMs = 0;
            iWaitMs = min(iWaitMs, iLightsMs);
        }

        m_Cond.wait_for(m_Lock, chrono::milliseconds(iWaitMs));
    }
    m_Lock.unlock();
}

void SMXManager::UpdatePanelTestMode()
{
    if(m_PanelTestMode == m_LastSentPanelTestMode &&
       (m_PanelTestMode == PanelTestMode_Off || GetMonotonicTime() - m_fLastPanelTestModeSentAt < PANEL_TEST_REFRESH_SECONDS))
        return;

    // When transitioning from Off to active, send a lights-off command to clear
    // panels before entering test mode.
    if(m_LastSentPanelTestMode == PanelTestMode_Off && m_PanelTestMode != PanelTestMode_Off)
    {
        string sCmd = "l";
        sCmd.append(LEGACY_LIGHTS_PAYLOAD_SIZE, '\0');
        sCmd.push_back('\n');
        for(auto &device : m_Devices)
            device.SendCommand(sCmd);
    }

    m_fLastPanelTestModeSentAt = GetMonotonicTime();
    m_LastSentPanelTestMode = m_PanelTestMode;
    for(auto &device : m_Devices)
        device.SendCommand(ssprintf("t %c\n", m_PanelTestMode));
}

void SMXManager::AttemptConnections()
{
    // Skip enumeration if both device slots are already occupied.
    if(!m_Devices[0].GetDevicePath().empty() && !m_Devices[1].GetDevicePath().empty())
        return;

    // Rate-limit enumeration to reduce syscalls.
    double fNow = GetMonotonicTime();
    if(fNow - m_fLastEnumerationTime < ENUMERATION_INTERVAL_SECONDS)
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
        for(const auto &device : m_Devices)
            if(device.GetDevicePath() == dev.sPath) { bOpen = true; break; }
        if(bOpen) continue;

        // Find an empty slot.
        SMXDevice *pSlot = nullptr;
        for(auto &device : m_Devices)
            if(device.GetDevicePath().empty()) { pSlot = &device; break; }

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

bool SMXManager::CorrectDeviceOrder()
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

        // Re-bind callbacks and pad indices after swap.
        m_Devices[0].SetConnectionCallbacks();
        m_Devices[1].SetConnectionCallbacks();
        m_Devices[0].SetPadIndex(0);
        m_Devices[1].SetPadIndex(1);
    }
    return bSwap;
}

} // namespace SMX
