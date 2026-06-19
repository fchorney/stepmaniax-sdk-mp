// SMX input resolution probe.
//
// Confirms the interrupt-driven read path resolves input at the USB frame floor
// (~1ms), now that each pad's poll thread blocks on the device and wakes the
// instant a report arrives. Runs in change-only mode, so the callback fires on
// real panel-state transitions: the ~10Hz idle heartbeat and same-state repeats
// are dropped, and every recorded event is a genuine input change.
//
// A human can't generate 1000 changes/sec, so this is not about report volume.
// It is about resolution: when two genuine changes land a frame apart (a jump
// hitting two panels, a fast roll, sensor flicker), do they arrive ~1ms apart
// rather than coalesced or delayed to the next heartbeat? The Min gap and the
// sub-2ms histogram buckets are the answer. Full Speed USB delivers one report
// per 1ms frame, so ~1ms is the floor regardless of firmware sampling rate.
//
// Build: cmake -B build -DBUILD_SAMPLE=ON && cmake --build build --target smx-input-timing
// Run:   ./build/smx-input-timing      (step on the pad, then Ctrl+C for the summary)

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>

#include "SMX.h"

using namespace std::chrono;

// 100us buckets covering 0..20ms.
static const size_t NUM_BUCKETS = 200;
static const uint64_t BUCKET_US = 100;

struct PadStats {
    uint64_t reports = 0;              // genuine input changes (change-only mode)
    steady_clock::time_point last_ts;
    bool has_last = false;
    // Inter-arrival interval distribution.
    std::array<uint64_t, NUM_BUCKETS> buckets{};
    uint64_t overflow = 0;
    uint64_t min_us = UINT64_MAX;
    uint64_t max_us = 0;
    uint64_t sum_us = 0;
    uint64_t interval_count = 0;
    // Rolling rate window.
    steady_clock::time_point window_start;
    uint64_t window_reports = 0;
    double rate_hz = 0.0;
};

static std::atomic<bool> g_running{true};
static std::mutex g_mutex;
static PadStats g_stats[2];
static bool g_connected[2] = {false, false};

static void OnStateChanged(const int pad, const SMXUpdateCallbackReason reason, void *)
{
    if(pad < 0 || pad > 1)
        return;

    if(SMX_REASON_IS(reason, SMXUpdateCallback_Connected))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_connected[pad] = true;
        g_stats[pad] = PadStats{};
        g_stats[pad].window_start = steady_clock::now();
        SMXInfo info;
        SMX_GetInfo(pad, &info);
        printf("Pad %d connected (P%d, fw %d)\n",
               pad, info.m_bIsPlayer2 ? 2 : 1, info.m_iFirmwareVersion);
    }
    if(SMX_REASON_IS(reason, SMXUpdateCallback_Disconnected))
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_connected[pad] = false;
        printf("Pad %d disconnected\n", pad);
    }
    if(SMX_REASON_IS(reason, SMXUpdateCallback_InputState))
    {
        const auto now = steady_clock::now();
        std::lock_guard<std::mutex> lock(g_mutex);
        PadStats &s = g_stats[pad];
        s.reports++;
        s.window_reports++;
        if(s.has_last)
        {
            // Record every inter-change interval, including sub-millisecond ones:
            // those are the resolution signal (two genuine changes a frame apart),
            // not noise to filter out.
            const uint64_t us = (uint64_t)duration_cast<microseconds>(now - s.last_ts).count();
            const size_t idx = (size_t)(us / BUCKET_US);
            if(idx < NUM_BUCKETS)
                s.buckets[idx]++;
            else
                s.overflow++;
            if(us < s.min_us) s.min_us = us;
            if(us > s.max_us) s.max_us = us;
            s.sum_us += us;
            s.interval_count++;
        }
        s.last_ts = now;
        s.has_last = true;

        // Refresh the rolling rate roughly twice a second.
        const double elapsed = duration<double>(now - s.window_start).count();
        if(elapsed >= 0.5)
        {
            s.rate_hz = (double)s.window_reports / elapsed;
            s.window_start = now;
            s.window_reports = 0;
        }
    }
}

static void PrintResults()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for(int pad = 0; pad < 2; pad++)
    {
        const PadStats &s = g_stats[pad];
        if(s.reports == 0)
            continue;

        printf("Pad %d:  %.0f changes/sec (%" PRIu64 " changes total)\n", pad, s.rate_hz, s.reports);

        if(s.min_us < UINT64_MAX)
            printf("  Min gap  : %6" PRIu64 " us  (%.0f Hz peak resolution)\n",
                   s.min_us, 1000000.0 / (double)std::max<uint64_t>(s.min_us, 1));
        if(s.interval_count > 0)
        {
            const double mean = (double)s.sum_us / (double)s.interval_count;
            printf("  Mean gap : %6.0f us  (%.0f Hz)\n", mean, 1000000.0 / mean);
        }
        if(s.max_us > 0)
            printf("  Max gap  : %6" PRIu64 " us\n", s.max_us);

        int last_bucket = -1;
        for(int i = (int)NUM_BUCKETS - 1; i >= 0; i--)
        {
            if(s.buckets[i] > 0) { last_bucket = i; break; }
        }

        if(last_bucket >= 0)
        {
            printf("\n  Inter-arrival histogram (%" PRIu64 "us buckets):\n", BUCKET_US);
            int show = last_bucket + 2;
            if(show > (int)NUM_BUCKETS) show = (int)NUM_BUCKETS;
            for(int i = 0; i < show; i++)
            {
                if(s.buckets[i] > 0)
                    printf("    %6.2fms  %" PRIu64 "\n",
                           (double)i * BUCKET_US / 1000.0, s.buckets[i]);
            }
            uint64_t overflow_total = s.overflow;
            for(int i = show; i < (int)NUM_BUCKETS; i++)
                overflow_total += s.buckets[i];
            if(overflow_total > 0)
                printf("    >%5.2fms  %" PRIu64 "\n",
                       (double)show * BUCKET_US / 1000.0, overflow_total);
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;

    signal(SIGINT,  [](int){ g_running = false; });
    signal(SIGTERM, [](int){ g_running = false; });

    printf("SMX Input Resolution Probe\n");
    printf("Change-only mode. A Min gap near ~1ms confirms input resolves at the USB frame floor.\n\n");

    SMX_Start(OnStateChanged, nullptr);
    SMX_SetMainThreadSleepMs(50);
    // Change-only mode (the default, set explicitly for clarity): only real
    // panel-state transitions are recorded, so the ~10Hz idle heartbeat and
    // same-state repeats don't pollute the histogram.
    SMX_SetInputStateMode(false);

    printf("Waiting for a pad");
    fflush(stdout);
    const auto deadline = steady_clock::now() + seconds(15);
    bool any = false;
    while(g_running && steady_clock::now() < deadline)
    {
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            any = g_connected[0] || g_connected[1];
        }
        if(any) break;
        std::this_thread::sleep_for(milliseconds(100));
        printf(".");
        fflush(stdout);
    }
    if(!any)
    {
        printf(" none found, exiting.\n");
        SMX_Stop();
        return 1;
    }
    printf("\n");

    printf("Step on the pad (lead with jumps and fast rolls to generate close changes)."
           " Press Ctrl+C to stop.\n");
    while(g_running)
        std::this_thread::sleep_for(milliseconds(100));
    printf("\n\n");

    SMX_Stop();
    PrintResults();
    return 0;
}
