// smx-bus-probe: triggers each internal bus command type for logic-analyzer capture.
//
// Connects to an SMX pad and provides an interactive menu to send individual
// command types to the MCU. The MCU forwards each to the panels over the
// internal UART bus. Use a logic analyzer (Saleae etc.) running continuously
// and note the printed timestamps to locate each command in the capture.
//
// Background: a 30Hz lights loop sends all-black data so the pad stays active
// (this produces the '4'/'2'/'3' triplet stream at baseline).
//
// Build:  cmake -DBUILD_SAMPLE=ON && make smx-bus-probe
// Run:    ./smx-bus-probe [pad_slot]   # pad_slot = 0 (default) or 1

#include <SMX.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono;

// --- Globals ----------------------------------------------------------------

volatile std::sig_atomic_t g_shouldExit = 0;
std::atomic<bool> g_connected{false};
std::atomic<int> g_targetPad{0};
std::atomic<bool> g_sendLights{false};

// 1350-byte buffer (2 pads x 9 panels x 25 LEDs x 3 RGB).
// Written by main thread; read by lights thread. Guarded by g_lightsMutex.
static char g_lightData[2 * SMX_BYTES_PER_PAD_25];
static std::mutex g_lightsMutex;

// --- Signal handler ---------------------------------------------------------

void signal_handler(int sig)
{
    if(sig == SIGINT)
        g_shouldExit = 1;
}

// --- SDK callback -----------------------------------------------------------

static void LogCallback(const char *log)
{
    fprintf(stderr, "[SMX] %s\n", log);
}

void OnStateChanged(int pad, SMXUpdateCallbackReason reason, void * /*pUser*/)
{
    if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected) && pad == g_targetPad.load())
    {
        SMXInfo info;
        SMX_GetInfo(pad, &info);
        printf("[connect] Pad %d: P%d, fw=%d, serial=%s\n",
            pad,
            info.m_bIsPlayer2 ? 2 : 1,
            info.m_iFirmwareVersion,
            info.m_bHasSerialNumber ? info.m_Serial : "(none)");
        g_connected.store(true);
    }
    else if(SMX_REASON_IS(reason, SMXUpdateCallback_Disconnected) && pad == g_targetPad.load())
    {
        printf("[disconnect] Pad %d\n", pad);
        g_connected.store(false);
        g_sendLights.store(false);
    }
}

// --- Light data helpers -----------------------------------------------------

// Fill one pad's slot in the global buffer with a single solid color.
void FillSolid(char *buf, int padIdx, uint8_t r, uint8_t g, uint8_t b)
{
    char *p = buf + padIdx * SMX_BYTES_PER_PAD_25;
    for(int i = 0; i < SMX_NUM_PANELS * SMX_LEDS_PER_PANEL_25; i++)
    {
        p[i * 3 + 0] = static_cast<char>(r);
        p[i * 3 + 1] = static_cast<char>(g);
        p[i * 3 + 2] = static_cast<char>(b);
    }
}

// Per-LED layout in the API buffer (25-LED mode, 75 bytes per panel):
//   Bytes  0-23  (LEDs  0- 7) : outer top half  -> bus command '2' -> GREEN
//   Bytes 24-47  (LEDs  8-15) : outer bot half  -> bus command '3' -> BLUE
//   Bytes 48-74  (LEDs 16-24) : inner 3x3 grid  -> bus command '4' -> RED
//
// After LED_COLOR_SCALE (0.6666), bus values are 170 for each channel.
void FillDistinctivePattern(char *buf, int padIdx)
{
    char *p = buf + padIdx * SMX_BYTES_PER_PAD_25;
    for(int panel = 0; panel < SMX_NUM_PANELS; panel++)
    {
        char *base = p + panel * SMX_LEDS_PER_PANEL_25 * 3;
        // LEDs 0-7 -> command '2' -> green
        for(int led = 0; led < 8; led++)
        {
            base[led * 3 + 0] = 0;
            base[led * 3 + 1] = static_cast<char>(255);
            base[led * 3 + 2] = 0;
        }
        // LEDs 8-15 -> command '3' -> blue
        for(int led = 8; led < 16; led++)
        {
            base[led * 3 + 0] = 0;
            base[led * 3 + 1] = 0;
            base[led * 3 + 2] = static_cast<char>(255);
        }
        // LEDs 16-24 -> command '4' -> red
        for(int led = 16; led < 25; led++)
        {
            base[led * 3 + 0] = static_cast<char>(255);
            base[led * 3 + 1] = 0;
            base[led * 3 + 2] = 0;
        }
    }
}

// --- Background lights thread -----------------------------------------------

void LightsThread()
{
    while(!g_shouldExit)
    {
        if(g_sendLights.load())
        {
            std::lock_guard<std::mutex> lock(g_lightsMutex);
            SMX_SetLights2(g_lightData, static_cast<int>(sizeof(g_lightData)));
        }
        std::this_thread::sleep_for(milliseconds(33));
    }
}

// --- Command handlers -------------------------------------------------------

static void CmdLightsDistinctive()
{
    printf("Sending distinctive lights for 3s:\n");
    printf("  bus '4' payload = RED   (inner 3x3, LEDs 16-24)\n");
    printf("  bus '2' payload = GREEN (outer top,  LEDs  0-7)\n");
    printf("  bus '3' payload = BLUE  (outer bot,  LEDs 8-15)\n");
    {
        std::lock_guard<std::mutex> lock(g_lightsMutex);
        FillDistinctivePattern(g_lightData, 0);
        FillDistinctivePattern(g_lightData, 1);
    }
    g_sendLights.store(true);
    printf("  [t=%.3f] pattern active\n", SMX_GetMonotonicTime());
    std::this_thread::sleep_for(seconds(3));
    {
        std::lock_guard<std::mutex> lock(g_lightsMutex);
        memset(g_lightData, 0, sizeof(g_lightData));
    }
    printf("  [t=%.3f] reverted to black\n", SMX_GetMonotonicTime());
}

static void CmdLightsBlack()
{
    printf("Sending all-black lights (bus '4'/'2'/'3' with all-zero payloads).\n");
    {
        std::lock_guard<std::mutex> lock(g_lightsMutex);
        memset(g_lightData, 0, sizeof(g_lightData));
    }
    g_sendLights.store(true);
    printf("  [t=%.3f] running\n", SMX_GetMonotonicTime());
}

static void CmdPlatformLights()
{
    // 264 bytes: 2 pads x 44 LEDs x 3 RGB. Fill both pads with solid white.
    char platBuf[2 * SMX_PLATFORM_STRIP_LEDS * 3];
    memset(platBuf, static_cast<uint8_t>(200), sizeof(platBuf));
    printf("Sending platform strip lights: all white (200,200,200).\n");
    printf("  USB: 'L'+strip_index+count+RGB; bus command unknown -- capture to find out.\n");
    SMX_SetPlatformLights(platBuf);
    printf("  [t=%.3f] sent\n", SMX_GetMonotonicTime());
    std::this_thread::sleep_for(seconds(2));
    memset(platBuf, 0, sizeof(platBuf));
    SMX_SetPlatformLights(platBuf);
    printf("  [t=%.3f] platform lights cleared\n", SMX_GetMonotonicTime());
}

static void CmdPanelTestOn()
{
    printf("Enabling panel test mode (pressure test).\n");
    printf("  SDK sends USB 't 1\\n'; MCU emits bus 'l'+zeros+'\\n' FIRST then 'T1' (refreshes ~1Hz).\n");
    g_sendLights.store(false);
    std::this_thread::sleep_for(milliseconds(100));
    SMX_SetPanelTestMode(PanelTestMode_PressureTest);
    printf("  [t=%.3f] panel test ON; holding 3s to capture at least 3 'T1' refreshes\n",
        SMX_GetMonotonicTime());
    std::this_thread::sleep_for(seconds(3));
    printf("  Done. Use option [4] to disable.\n");
}

static void CmdPanelTestOff()
{
    printf("Disabling panel test mode.\n");
    printf("  SDK sends USB 't 0\\n'; MCU emits bus 'T0'.\n");
    SMX_SetPanelTestMode(PanelTestMode_Off);
    printf("  [t=%.3f] panel test OFF; resuming lights\n", SMX_GetMonotonicTime());
    g_sendLights.store(true);
}

static void CmdSensorTest(int pad, SensorTestMode mode, const char *modeName, char modeChar)
{
    // The middle byte of the bus command 'BXP' is assumed to mirror the USB
    // mode character, but this is unconfirmed -- that's what we're capturing.
    printf("Sensor test: %s (USB 'y%c\\n'; bus expected 'B%cP' -- unconfirmed).\n",
        modeName, modeChar, modeChar);
    printf("  Runs for 3s (~90 requests at 30Hz) then stops.\n");
    SMX_SetTestMode(pad, mode);
    printf("  [t=%.3f] started\n", SMX_GetMonotonicTime());
    std::this_thread::sleep_for(seconds(3));
    SMX_SetTestMode(pad, SensorTestMode_Off);
    printf("  [t=%.3f] stopped\n", SMX_GetMonotonicTime());
}

static void CmdConfigWrite(int pad)
{
    SMXConfig config;
    if(!SMX_GetConfig(pad, &config))
    {
        printf("Cannot read config: pad %d not connected.\n", pad);
        return;
    }
    printf("Config write: reading current config and writing it back unchanged.\n");
    printf("  USB: 'W'+size+SMXConfig (fw>=5); bus: 'w'+0xFA+<unknown>+config.\n");
    printf("  Rate-limited to 1 write/s; expect up to 1s of delay.\n");
    SMX_SetConfig(pad, &config);
    printf("  [t=%.3f] queued; waiting 2s for transmission\n", SMX_GetMonotonicTime());
    std::this_thread::sleep_for(seconds(2));
    printf("  [t=%.3f] should have been sent\n", SMX_GetMonotonicTime());
}

static void CmdReenableAutoLights()
{
    printf("Re-enable auto lights (USB: 'S 1\\n'; bus effect unknown -- capture to find out).\n");
    SMX_ReenableAutoLights();
    printf("  [t=%.3f] sent\n", SMX_GetMonotonicTime());
}

static void CmdForceRecalibration(int pad)
{
    printf("Force recalibration (USB: 'C\\n'; bus: likely 'U' -- unconfirmed).\n");
    SMX_ForceRecalibration(pad);
    printf("  [t=%.3f] sent\n", SMX_GetMonotonicTime());
}

// --- Menu -------------------------------------------------------------------

static void PrintMenu(int pad)
{
    printf("\n=== SMX Bus Probe (pad slot %d) ===\n", pad);
    printf("  [1]  Lights: distinctive pattern  -> bus '4'=red / '2'=green / '3'=blue\n");
    printf("  [2]  Lights: all black             -> bus '4'/'2'/'3' with zero payloads\n");
    printf("  [3]  Platform strip lights (white) -> bus command unknown\n");
    printf("  [4]  Panel test mode ON            -> bus 'l'+zeros then 'T1' @1Hz\n");
    printf("  [5]  Panel test mode OFF           -> bus 'T0'\n");
    printf("  [6]  Sensor test: uncalibrated     -> bus 'B?P' (mode char '0')\n");
    printf("  [7]  Sensor test: calibrated       -> bus 'B1P' (confirmed)\n");
    printf("  [8]  Sensor test: noise            -> bus 'B?P' (mode char '2')\n");
    printf("  [9]  Sensor test: tare             -> bus 'B?P' (mode char '3')\n");
    printf("  [10] Config write                  -> bus 'w'+0xFA+?+SMXConfig\n");
    printf("  [11] Re-enable auto lights         -> bus effect unknown\n");
    printf("  [12] Force recalibration           -> bus likely 'U'\n");
    printf("  [q]  Quit\n");
    printf("> ");
    fflush(stdout);
}

// --- Main -------------------------------------------------------------------

int main(int argc, char *argv[])
{
    std::signal(SIGINT, signal_handler);

    int iPad = 0;
    for(int i = 1; i < argc; i++)
    {
        const std::string arg(argv[i]);
        if(arg == "1" || arg == "--pad=1")
            iPad = 1;
    }
    g_targetPad.store(iPad);

    printf("SMX Bus Probe v%s\n", SMX_Version());
    printf("Targeting pad slot %d. Waiting for connection...\n", iPad);

    memset(g_lightData, 0, sizeof(g_lightData));

    SMX_SetLogCallback(LogCallback);
    SMX_Start(OnStateChanged, nullptr);

    std::thread lightsThread(LightsThread);

    while(!g_shouldExit && !g_connected.load())
        std::this_thread::sleep_for(milliseconds(100));

    if(g_shouldExit)
    {
        g_shouldExit = 1;
        lightsThread.join();
        SMX_Stop();
        return 0;
    }

    g_sendLights.store(true);

    while(!g_shouldExit)
    {
        if(!g_connected.load())
        {
            printf("Pad %d disconnected. Waiting for reconnect...\n", iPad);
            while(!g_shouldExit && !g_connected.load())
                std::this_thread::sleep_for(milliseconds(100));
            if(g_shouldExit)
                break;
            printf("Pad %d reconnected.\n", iPad);
            g_sendLights.store(true);
        }

        PrintMenu(iPad);

        std::string line;
        if(!std::getline(std::cin, line))
            break;
        if(line.empty())
            continue;

        if(!g_connected.load())
            continue;

        if(line == "q" || line == "Q")
            break;
        else if(line == "1")
            CmdLightsDistinctive();
        else if(line == "2")
            CmdLightsBlack();
        else if(line == "3")
            CmdPlatformLights();
        else if(line == "4")
            CmdPanelTestOn();
        else if(line == "5")
            CmdPanelTestOff();
        else if(line == "6")
            CmdSensorTest(iPad, SensorTestMode_UncalibratedValues, "uncalibrated", '0');
        else if(line == "7")
            CmdSensorTest(iPad, SensorTestMode_CalibratedValues, "calibrated", '1');
        else if(line == "8")
            CmdSensorTest(iPad, SensorTestMode_Noise, "noise", '2');
        else if(line == "9")
            CmdSensorTest(iPad, SensorTestMode_Tare, "tare", '3');
        else if(line == "10")
            CmdConfigWrite(iPad);
        else if(line == "11")
            CmdReenableAutoLights();
        else if(line == "12")
            CmdForceRecalibration(iPad);
        else
            printf("Unknown option '%s'.\n", line.c_str());
    }

    printf("Shutting down...\n");
    g_sendLights.store(false);
    SMX_SetPanelTestMode(PanelTestMode_Off);
    for(int pad = 0; pad < 2; pad++)
        SMX_SetTestMode(pad, SensorTestMode_Off);
    g_shouldExit = 1;
    lightsThread.join();
    SMX_Stop();
    return 0;
}
