// USB input timing probe.
//
// Tracks genuine input state changes and histograms inter-change intervals.
// Gaps below BURST_THRESHOLD_US are OS drain artifacts (two USB reports
// buffered and read back-to-back, appearing microseconds apart instead of
// ~1ms apart). These are counted separately and excluded from the histogram.
//
// Full Speed USB delivers one HID report per 1ms frame -- that is the hard
// precision floor for step timing regardless of firmware sampling rate. The
// histogram shows where genuine inter-change intervals land relative to that
// floor.
//
// Build: cmake -B build -DBUILD_SAMPLE=ON && cmake --build build --target smx-input-timing
// Run:   ./build/smx-input-timing [poll_sleep_us]
//
// poll_sleep_us: USB poll thread sleep in microseconds (default: 500)
//   Lower = less latency between USB report arrival and detection.
//   2000Hz (500us) is a good default: polls twice per USB frame for
//   max 1.5ms total latency with minimal CPU overhead.
//   Diminishing returns past 4000Hz (250us) since USB jitter dominates.
//
// Press Ctrl+C to stop and print the session summary.

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

static const size_t NUM_BUCKETS = 200;
static const uint64_t BUCKET_US = 100;
static const uint64_t BURST_THRESHOLD_US = 1100;

struct PadStats {
    uint64_t changes = 0;
    steady_clock::time_point last_ts;
    bool has_last = false;
    std::array<uint64_t, NUM_BUCKETS> buckets{};
    uint64_t overflow = 0;
    uint64_t min_us = UINT64_MAX;
    uint64_t max_us = 0;
    uint64_t sum_us = 0;
    uint64_t interval_count = 0;
    uint64_t burst_count = 0;
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
        s.changes++;
        if(s.has_last)
        {
            const uint64_t us = (uint64_t)duration_cast<microseconds>(now - s.last_ts).count();
            if(us < BURST_THRESHOLD_US)
            {
                s.burst_count++;
            }
            else
            {
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
        }
        s.last_ts = now;
        s.has_last = true;
    }
}

static void PrintResults()
{
    std::lock_guard<std::mutex> lock(g_mutex);
    for(int pad = 0; pad < 2; pad++)
    {
        const PadStats &s = g_stats[pad];
        if(s.changes == 0)
            continue;

        printf("Pad %d:  (%" PRIu64 " state changes, %" PRIu64 " genuine intervals,"
               " %" PRIu64 " drain artifacts filtered)\n",
               pad, s.changes, s.interval_count, s.burst_count);

        if(s.min_us < UINT64_MAX)
            printf("  Min gap  : %6" PRIu64 " us\n", s.min_us);
        if(s.interval_count > 0)
            printf("  Mean gap : %6.0f us\n", (double)s.sum_us / (double)s.interval_count);
        if(s.max_us > 0)
            printf("  Max gap  : %6" PRIu64 " us\n", s.max_us);

        int last_bucket = -1;
        for(int i = (int)NUM_BUCKETS - 1; i >= 0; i--)
        {
            if(s.buckets[i] > 0)
            {
                last_bucket = i;
                break;
            }
        }

        if(last_bucket >= 0)
        {
            printf("\n  Histogram (%" PRIu64 "us buckets):\n", BUCKET_US);
            int show = last_bucket + 3;
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

    printf("SMX Input Timing Probe\n");
    printf("Input reads are interrupt-driven (each pad's poll thread blocks on the device).\n");
    printf("Precision floor: ~1ms (Full Speed USB frame)."
           " Gaps < %" PRIu64 "us are drain artifacts.\n\n",
           BURST_THRESHOLD_US);

    SMX_Start(OnStateChanged, nullptr);
    SMX_SetMainThreadSleepMs(50);

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

    printf("Step rapidly on any panel. Press Ctrl+C to stop.\n");
    while(g_running)
        std::this_thread::sleep_for(milliseconds(100));
    printf("\n\n");

    SMX_Stop();
    PrintResults();
    return 0;
}
