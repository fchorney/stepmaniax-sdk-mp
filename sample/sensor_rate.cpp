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
// Run:   ./build/smx-sensor-rate [phase_secs] [lights_hz] [pad]
//
// [pad] (0 or 1) limits sensor test mode to a single pad. The other connected
// pad is then left untouched and acts as a visual control: light frames still
// reach it during the on phase (SMX_SetLights2 covers both pads), but in the off
// phase it reverts to its firmware idle animation, showing that the host stops
// driving lights. A pad that is itself being measured stays in sensor test mode
// the whole run and holds its last frame, so it will not show the idle animation.

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

// A frame that lights one panel (moving each tick) on both pads. The motion is
// what makes light lag/backlog visible: if frames are starved, the lit panel
// stutters or keeps moving after the phase ends instead of tracking in real time.
static void MovingFrame(std::string &buf, int tick)
{
    const int BYTES_PER_PAD = 675;   // 9 panels * 25 LEDs * 3 (RGB)
    const int BYTES_PER_PANEL = 75;
    buf.assign(LIGHTS_BYTES, char(0));
    const int panel = tick % 9;
    for(int pad = 0; pad < 2; ++pad)
    {
        const int base = pad * BYTES_PER_PAD + panel * BYTES_PER_PANEL;
        for(int b = 0; b < BYTES_PER_PANEL; ++b)
            buf[base + b] = char(96);
    }
}

// Measures samples/sec per active pad over the given window, optionally streaming
// a moving light pattern at lightsHz. Prints one line per active pad.
static void RunPhase(const char *label, int secs, int lightsHz, bool lights, const bool active[2])
{
    printf("Phase: %s for %ds ...\n", label, secs);
    for(int i = 0; i < 2; i++)
        g_samples[i].store(0);

    std::string lightData;
    const auto start = steady_clock::now();
    const auto dur = seconds(secs);
    const auto frame = duration_cast<steady_clock::duration>(
        duration<double>(lightsHz > 0 ? 1.0 / lightsHz : 0.05));
    int tick = 0;
    long framesSent = 0;
    auto next = steady_clock::now();
    while(steady_clock::now() - start < dur)
    {
        if(lights)
        {
            MovingFrame(lightData, tick++);
            SMX_SetLights2(lightData.data(), LIGHTS_BYTES);
            framesSent++;
        }
        // Deadline-based pacing so SMX_SetLights2 cost doesn't drag the rate down.
        next += frame;
        const auto now = steady_clock::now();
        if(next > now)
            std::this_thread::sleep_for(next - now);
    }

    const double elapsed = duration_cast<duration<double>>(steady_clock::now() - start).count();
    if(lights)
        printf("  (streamed %ld light frames = %.1f/s requested)\n", framesSent, framesSent / elapsed);
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
    const int targetPad = (argc > 3 && (atoi(argv[3]) == 0 || atoi(argv[3]) == 1)) ? atoi(argv[3]) : -1;

    printf("SMX sensor sampling-rate probe\n");
    printf("phase length: %ds per phase, light rate: %dHz\n", phaseSecs, lightsHz);
    if(targetPad >= 0)
        printf("measuring pad %d only (other connected pad is a visual control)\n\n", targetPad);
    else
        printf("measuring all connected pads\n\n");

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

    // A pad is measured if it is connected and not excluded by the pad selector.
    bool active[2] = {
        g_connected[0].load() && (targetPad < 0 || targetPad == 0),
        g_connected[1].load() && (targetPad < 0 || targetPad == 1),
    };
    for(int i = 0; i < 2; i++)
    {
        if(active[i])
        {
            printf(" ... measuring pad %i.\n", i);
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
