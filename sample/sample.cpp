#include <csignal>
#include <cstdlib>
#include <iostream>
#include <thread>

#include "SMX.h"


volatile std::sig_atomic_t g_shouldExit = 0;

void CustomLogCallback(const char *log)
{
    std::cerr << "[SMX] " << log << std::endl;
}

void signal_handler(const int signal)
{
    if(signal == SIGINT)
        g_shouldExit = 1;
}

void OnStateChanged(const int pad, const SMXUpdateCallbackReason reason, void *pUser)
{
    if(SMX_REASON_IS(reason, SMXUpdateCallback_Disconnected))
    {
        printf("Pad %i: disconnected\n", pad);
        return;
    }

    if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
    {
        SMXInfo info;
        SMX_GetInfo(pad, &info);
        printf("Pad %i connected (jumper: P%i, serial: %s, fw: %i)\n",
            pad,
            info.m_bIsPlayer2 ? 2 : 1,
            info.m_bHasSerialNumber ? info.m_Serial : "(none)",
            info.m_iFirmwareVersion);

        if(!info.m_bHasSerialNumber)
            printf("Warning: Pad %i has no serial number. Call SMX_SetSerialNumbers() to assign one.\n", pad);

        return;
    }

    if(SMX_REASON_IS(reason, SMXUpdateCallback_InputState))
    {
        const uint16_t state = SMX_GetInputState(pad);
        printf("%6.3f: ", SMX_GetMonotonicTime());
        printf("Pad %i: input state %04x\n", pad, state);
    }
}

int main(int argc, char *argv[])
{
    std::signal(SIGINT, signal_handler);
    SMX_SetLogCallback(CustomLogCallback);

    printf("SMX SDK Multi Platform v%s\n", SMX_Version());
    SMX_Start(OnStateChanged, nullptr);

    bool bAllPackets = false;
    bool bTestMode = false;
    SensorTestMode sensorMode = SensorTestMode_Off;
    for(int i = 1; i < argc; i++)
    {
        if(std::string(argv[i]) == "--all-packets")
            bAllPackets = true;
        else if(std::string(argv[i]) == "--test-mode")
            bTestMode = true;
        else if(std::string(argv[i]) == "--uncalibrated")
            sensorMode = SensorTestMode_UncalibratedValues;
        else if(std::string(argv[i]) == "--calibrated")
            sensorMode = SensorTestMode_CalibratedValues;
        else if(std::string(argv[i]) == "--noise")
            sensorMode = SensorTestMode_Noise;
        else if(std::string(argv[i]) == "--tare")
            sensorMode = SensorTestMode_Tare;
    }

    if(argc >= 2 && argv[1][0] != '-')
    {
        int mainMs = atoi(argv[1]);
        int usbUs = argc >= 3 && argv[2][0] != '-' ? atoi(argv[2]) : 1000;
        SMX_SetPollingRate(mainMs, usbUs);
        printf("Polling rate: main thread %dms, USB thread %dus\n", mainMs, usbUs);
    }

    if(bAllPackets)
    {
        SMX_SetInputStateMode(true);
        printf("Input state mode: fire on every Report 3 packet\n");
    }

    if(bTestMode)
    {
        SMX_SetPanelTestMode(PanelTestMode_PressureTest);
        printf("Panel test mode: pressure test enabled\n");
    }

    if(sensorMode != SensorTestMode_Off)
    {
        const char *modeNames[] = { "", "uncalibrated", "calibrated", "noise", "tare" };
        int modeIdx = sensorMode == SensorTestMode_UncalibratedValues ? 1 :
                      sensorMode == SensorTestMode_CalibratedValues ? 2 :
                      sensorMode == SensorTestMode_Noise ? 3 : 4;
        printf("Sensor test mode: %s\n", modeNames[modeIdx]);
        for(int pad = 0; pad < 2; pad++)
            SMX_SetTestMode(pad, sensorMode);
    }

    printf("Scanning for StepManiaX devices... Press Ctrl+C to quit.\n");
    printf("Usage: %s [main_thread_ms] [usb_polling_us] [--all-packets] [--test-mode]\n", argv[0]);
    printf("       [--uncalibrated] [--calibrated] [--noise] [--tare]\n");

    while(!g_shouldExit)
    {
        if(sensorMode != SensorTestMode_Off)
        {
            for(int pad = 0; pad < 2; pad++)
            {
                SMXSensorTestModeData data = {};
                if(!SMX_GetTestData(pad, &data))
                    continue;
                printf("%6.3f: Pad %i sensors:", SMX_GetMonotonicTime(), pad);
                for(int panel = 0; panel < 9; panel++)
                {
                    if(!data.bHaveDataFromPanel[panel])
                    {
                        printf(" [--]");
                        continue;
                    }
                    printf(" [%d,%d,%d,%d]",
                        data.sensorLevel[panel][0], data.sensorLevel[panel][1],
                        data.sensorLevel[panel][2], data.sensorLevel[panel][3]);
                }
                printf("\n");
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if(sensorMode != SensorTestMode_Off)
        for(int pad = 0; pad < 2; pad++)
            SMX_SetTestMode(pad, SensorTestMode_Off);

    if(bTestMode)
        SMX_SetPanelTestMode(PanelTestMode_Off);

    SMX_Stop();
    return 0;
}
