// Sensor sampling-rate probe (run against a real pad).
//
// Enables calibrated sensor test mode on every connected pad, then measures how
// many sensor samples per second arrive (a) while streaming light frames at 30Hz
// like a game does, and (b) with no light traffic at all. Prints per-pad rates so
// the light-contention effect (and any fix for it) is visible without wiring the
// SDK up to a game. Running with two pads connected also shows whether their
// throughput is independent (each pad has its own command pipeline).
//
// Build: cmake -B build -DBUILD_SAMPLE=ON && cmake --build build --target smx-sensor-rate
// Run:   ./build/smx-sensor-rate [phase_secs] [lights_hz]

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "SMX.h"

using namespace std::chrono;

// Two pads worth of 25-LED light data (SMX_BYTES_PER_PAD_25 = 675). Matches the
// frame size a game streams; exact LED values do not matter for timing.
static const int LIGHTS_BYTES = 2 * SMX_BYTES_PER_PAD_25;

static std::atomic<unsigned> g_samples[2];
static std::atomic<bool> g_connected[2];

static void OnStateChanged(const int pad, const SMXUpdateCallbackReason reason, void *)
{
    if(pad < 0 || pad > 1)
        return;
    if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
    {
        SMXInfo info;
        SMX_GetInfo(pad, &info);
        printf("Pad %i connected (P%i, fw %i)\n", pad, info.m_bIsPlayer2 ? 2 : 1, info.m_iFirmwareVersion);
        g_connected[pad].store(true);
    }
    if(SMX_REASON_IS(reason, SMXUpdateCallback_Disconnected))
        g_connected[pad].store(false);
    if(SMX_REASON_IS(reason, SMXUpdateCallback_SensorTestData))
        g_samples[pad].fetch_add(1);
}

// Measures samples/sec per active pad over the given window, optionally streaming
// light frames at lightsHz. Prints one line per active pad.
static void RunPhase(const char *label, int secs, int lightsHz, bool lights, const bool active[2])
{
    printf("Phase: %s for %ds ...\n", label, secs);
    if(!lights)
    {
        // Visibly clear the panels for the off phase: one frame, not streamed, so
        // it adds no ongoing light traffic. The pad may re-assert its own idle
        // lighting afterward, which is on-device and does not contend with sensor
        // polling.
        std::string off(LIGHTS_BYTES, char(0));
        SMX_SetLights2(off.data(), LIGHTS_BYTES);
    }
    for(int i = 0; i < 2; i++)
        g_samples[i].store(0);

    std::string lightData(LIGHTS_BYTES, char(32));
    const auto start = steady_clock::now();
    const auto dur = seconds(secs);
    const auto frame = milliseconds(lightsHz > 0 ? (1000 / lightsHz) : 50);
    while(steady_clock::now() - start < dur)
    {
        if(lights)
            SMX_SetLights2(lightData.data(), LIGHTS_BYTES);
        std::this_thread::sleep_for(frame);
    }

    const double elapsed = duration_cast<duration<double>>(steady_clock::now() - start).count();
    for(int i = 0; i < 2; i++)
    {
        if(!active[i])
            continue;
        const unsigned count = g_samples[i].load();
        printf("  pad %i -> %u samples in %.2fs = %.1f/s\n", i, count, elapsed, count / elapsed);
    }
}

int main(int argc, char **argv)
{
    const int phaseSecs = argc > 1 ? atoi(argv[1]) : 6;
    const int lightsHz = argc > 2 ? atoi(argv[2]) : 30;

    printf("SMX sensor sampling-rate probe\n");
    printf("phase length: %ds per phase, light rate: %dHz\n\n", phaseSecs, lightsHz);

    SMX_Start(OnStateChanged, nullptr);

    printf("Waiting for a pad");
    const auto deadline = steady_clock::now() + seconds(15);
    while(!g_connected[0].load() && !g_connected[1].load())
    {
        if(steady_clock::now() > deadline)
        {
            printf(" ... none found, exiting.\n");
            SMX_Stop();
            return 1;
        }
        std::this_thread::sleep_for(milliseconds(100));
    }
    std::this_thread::sleep_for(milliseconds(300)); // let a second pad enumerate

    bool active[2] = { g_connected[0].load(), g_connected[1].load() };
    for(int i = 0; i < 2; i++)
    {
        if(active[i])
        {
            printf(" ... pad %i active.\n", i);
            SMX_SetTestMode(i, SensorTestMode_CalibratedValues);
        }
    }
    std::this_thread::sleep_for(seconds(1)); // settle

    RunPhase("LIGHTS ON ", phaseSecs, lightsHz, true, active);
    RunPhase("LIGHTS OFF", phaseSecs, 0, false, active);

    for(int i = 0; i < 2; i++)
    {
        if(active[i])
            SMX_SetTestMode(i, SensorTestMode_Off);
    }
    SMX_Stop();
    return 0;
}
