// SMX SDK Multi Platform — Public C API
// This file contains the exported C API functions and the test-only internal API.

#include "SMX.h"

#include <memory>
#include <string>

#include "SMXHelpers.h"
#include "SMXManager.h"
#include "SMXProtocolConstants.h"

#include "SMXVersion.h"

using namespace std;
using namespace SMX;

// File-static singleton. No global variable visible outside this file.
static unique_ptr<SMXManager> g_pSMX;

// Declared in SMXPanelAnimation.cpp
void SMXLightsAnimation_TemporaryStop();

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

SMX_API void SMX_Start(SMXUpdateCallback callback, void *pUser)
{
    if(g_pSMX)
    {
        Log("SMX_Start called while already running; ignoring.");
        return;
    }

    auto cb = [callback, pUser](const int pad, const SMXUpdateCallbackReason reason) {
        callback(pad, reason, pUser);
    };
    g_pSMX = make_unique<SMXManager>(cb);
}

SMX_API void SMX_Stop()
{
    SMX_LightsAnimation_SetAuto(false);
    g_pSMX.reset();
}

SMX_API void SMX_SetLogCallback(SMXLogCallback callback)
{
    SetLogCallback(callback);
}

SMX_API void SMX_GetInfo(const int pad, SMXInfo *info)
{
    if(!g_pSMX) return;
    const auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->GetInfo(*info);
}

SMX_API bool SMX_GetConfig(const int pad, SMXConfig *config)
{
    if(!g_pSMX || !config) return false;
    const auto *dev = g_pSMX->GetDevice(pad);
    if(!dev) return false;
    return dev->GetConfig(*config);
}

SMX_API void SMX_SetConfig(const int pad, const SMXConfig *config)
{
    if(!g_pSMX || !config) return;
    auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->SetConfig(*config);
}

SMX_API uint16_t SMX_GetInputState(const int pad)
{
    if(!g_pSMX) return 0;
    const auto *dev = g_pSMX->GetDevice(pad);
    return dev ? dev->GetInputState() : 0;
}

SMX_API void SMX_SetSerialNumbers()
{
    if(g_pSMX) g_pSMX->SetSerialNumbers();
}

SMX_API void SMX_FactoryReset(const int pad)
{
    if(!g_pSMX) return;
    auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->FactoryReset();
}

SMX_API void SMX_ForceRecalibration(const int pad)
{
    if(!g_pSMX) return;
    auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->ForceRecalibration();
}

SMX_API void SMX_ReenableAutoLights()
{
    if(g_pSMX) g_pSMX->ReenableAutoLights();
}

SMX_API void SMX_SetLights2(const char *lightData, int lightDataSize)
{
    if(!g_pSMX || !lightData) return;

    if(lightDataSize != 2*BYTES_PER_PAD_16 && lightDataSize != 2*BYTES_PER_PAD_25)
    {
        Log(ssprintf("SMX_SetLights2: lightDataSize must be %i or %i, got %i",
            2*BYTES_PER_PAD_16, 2*BYTES_PER_PAD_25, lightDataSize));
        return;
    }

    g_pSMX->SetLights(lightData, lightDataSize);

    // Pause auto-animation briefly so it doesn't compete with direct lights control.
    SMXLightsAnimation_TemporaryStop();
}

SMX_API void SMX_SetLights(const char lightData[864])
{
    SMX_SetLights2(lightData, 864);
}

SMX_API void SMX_SetPlatformLights(const char *pLightData)
{
    if(!g_pSMX || !pLightData) return;
    g_pSMX->SetPlatformLights(pLightData);
}

SMX_API void SMX_SetPanelTestMode(PanelTestMode mode)
{
    if(g_pSMX) g_pSMX->SetPanelTestMode(mode);
}

SMX_API void SMX_SetTestMode(const int pad, SensorTestMode mode)
{
    if(!g_pSMX) return;
    auto *dev = g_pSMX->GetDevice(pad);
    if(dev) dev->SetSensorTestMode(mode);
}

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

SMX_API const char *SMX_Version()
{
    return SMX_VERSION;
}

SMX_API double SMX_GetMonotonicTime()
{
    return GetMonotonicTime();
}

// ---------------------------------------------------------------------------
// Test-only API (not exported from shared library, linked directly in tests)
// ---------------------------------------------------------------------------

/// Sends a command to a specific pad. Used by SMXPanelAnimation.cpp for upload.
void SMX_SendCommandForPad(int pad, const std::string &cmd, std::function<void(std::string)> pComplete)
{
    if(!g_pSMX) { if(pComplete) pComplete(""); return; }
    auto *dev = g_pSMX->GetDevice(pad);
    if(!dev) { if(pComplete) pComplete(""); return; }
    dev->SendCommand(cmd, pComplete);
}

/// Starts the SDK with a custom HID enumerator for testing.
void SMX_StartWithEnumerator(SMXUpdateCallback callback, void *pUser, std::unique_ptr<SMX::IHIDEnumerator> pEnumerator)
{
    if(g_pSMX) return;

    auto cb = [callback, pUser](const int pad, const SMXUpdateCallbackReason reason) {
        callback(pad, reason, pUser);
    };
    g_pSMX = make_unique<SMXManager>(cb, std::move(pEnumerator));
}
