// Read/write HID handle probe.
//
// Validates, on real hardware, the two assumptions behind splitting the SMX
// connection into a dedicated read handle and write handle:
//
//   1. The device path can be opened TWICE (a second hidapi handle).
//   2. With two independent handles, a read() never waits behind a blocking
//      write(), so input reads stay fast while lights/sensor writes are in
//      flight.
//
// It talks to hidapi directly (not SMXManager), so it measures the raw platform
// behaviour. Two modes:
//
//   (default) two-handle: open the path twice; read and write on separate
//             handles with no shared lock.
//   `single`: open the path once and share it between the reader and writer
//             behind a mutex, like the pre-split SDK. This is the control: the
//             slowest read() call should jump up to ~the slowest write, since a
//             read has to wait for the in-flight write to release the lock.
//
// Pads emit input only on change plus a ~10Hz heartbeat, so most reads return no
// data. We therefore time the read() CALL itself, not the gap between
// data-bearing reads (which would just measure the pad's output cadence).
//
// Build with -DBUILD_SAMPLE=ON, then on a machine with a pad plugged in:
//   ./smx-rw-probe            # two-handle
//   ./smx-rw-probe single     # single-handle control

#include <SMX.h>

#include <hidapi/hidapi.h>
#if defined(__APPLE__)
#include <hidapi/hidapi_darwin.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono;

namespace {

const size_t HID_PACKET_SIZE = 64;
const uint8_t HID_REPORT_COMMAND = 0x05;
const uint8_t HID_REPORT_INPUT_STATE = 0x03;
const uint8_t PACKET_FLAG_DEVICE_INFO = 0x80;
const int RUN_SECONDS = 8;

uint64_t NowNs()
{
    return static_cast<uint64_t>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

void AtomicMax(std::atomic<uint64_t> &dst, uint64_t value)
{
    uint64_t cur = dst.load(std::memory_order_relaxed);
    while(value > cur && !dst.compare_exchange_weak(cur, value, std::memory_order_relaxed))
    {
    }
}

// Runs the concurrent reader + writer for RUN_SECONDS and prints the result.
// doRead/doWrite abstract over the one-handle vs two-handle wiring; the timing
// around them is identical, so the only variable is the locking.
void RunProbe(const char *label,
              const std::function<int(uint8_t *, size_t)> &doRead,
              const std::function<int(const uint8_t *, size_t)> &doWrite)
{
    std::atomic<bool> stop{false};
    std::atomic<uint64_t> readAttempts{0};
    std::atomic<uint64_t> readsWithData{0};
    std::atomic<uint64_t> inputReports{0};
    // The key metric: the longest a single read() call took (lock wait included
    // in single-handle mode). With independent handles it should stay in
    // microseconds; if reads are coupled to writes it tracks the slowest write.
    std::atomic<uint64_t> maxReadCallUs{0};

    std::thread reader([&]() {
        uint8_t buf[HID_PACKET_SIZE];
        while(!stop.load(std::memory_order_relaxed))
        {
            const uint64_t t = NowNs();
            const int res = doRead(buf, HID_PACKET_SIZE);
            AtomicMax(maxReadCallUs, (NowNs() - t) / 1000);
            readAttempts.fetch_add(1, std::memory_order_relaxed);
            if(res > 0)
            {
                readsWithData.fetch_add(1, std::memory_order_relaxed);
                if(buf[0] == HID_REPORT_INPUT_STATE)
                    inputReports.fetch_add(1, std::memory_order_relaxed);
            }
            else if(res < 0)
            {
                fprintf(stderr, "read error\n");
                break;
            }
        }
    });

    // Writer: hammer continuously (no pause) so a large share of the reader's
    // read() calls overlap an in-flight write. Time the slowest single write to
    // show writes really do block.
    printf("Running concurrent read+write for %ds (press the pad to generate input)...\n", RUN_SECONDS);
    uint8_t req[HID_PACKET_SIZE];
    memset(req, 0, sizeof(req));
    req[0] = HID_REPORT_COMMAND;
    req[1] = PACKET_FLAG_DEVICE_INFO;

    uint64_t writes = 0;
    uint64_t writeErrors = 0;
    uint64_t maxWriteUs = 0;
    const auto deadline = steady_clock::now() + seconds(RUN_SECONDS);
    while(steady_clock::now() < deadline)
    {
        const uint64_t t = NowNs();
        if(doWrite(req, HID_PACKET_SIZE) < 0)
            writeErrors++;
        const uint64_t us = (NowNs() - t) / 1000;
        if(us > maxWriteUs)
            maxWriteUs = us;
        writes++;
    }

    stop.store(true, std::memory_order_relaxed);
    reader.join();

    const double secs = RUN_SECONDS;
    printf("\n=== rw-probe results [%s] ===\n", label);
    printf("writes:               %llu (%.0f/s), errors %llu, slowest write %.2fms\n",
           static_cast<unsigned long long>(writes), writes / secs,
           static_cast<unsigned long long>(writeErrors), maxWriteUs / 1000.0);
    printf("read attempts:        %llu (%.0f/s), with data %llu, input-state reports %llu\n",
           static_cast<unsigned long long>(readAttempts.load()), readAttempts.load() / secs,
           static_cast<unsigned long long>(readsWithData.load()),
           static_cast<unsigned long long>(inputReports.load()));
    printf("slowest read() call:  %.3fms  <-- KEY: ~microseconds = reads independent of writes;\n",
           maxReadCallUs.load() / 1000.0);
    printf("                                    near the slowest write = reads coupled to writes.\n");
}

} // namespace

int main(int argc, char **argv)
{
    bool single = false;
    for(int i = 1; i < argc; i++)
    {
        const std::string arg(argv[i]);
        if(arg == "single" || arg == "--single")
            single = true;
    }

    if(hid_init() != 0)
    {
        fprintf(stderr, "hid_init failed\n");
        return 1;
    }
#if defined(__APPLE__)
    // macOS opens IOHIDDevice exclusively by default; allow the second open.
    hid_darwin_set_open_exclusive(0);
#endif

    std::string path;
    hid_device_info *devs = hid_enumerate(SMX_USB_VENDOR_ID, SMX_USB_PRODUCT_ID);
    for(hid_device_info *cur = devs; cur; cur = cur->next)
    {
        if(cur->path)
        {
            path = cur->path;
            break;
        }
    }
    hid_free_enumeration(devs);
    if(path.empty())
    {
        fprintf(stderr, "No StepManiaX device found. Plug a pad in and retry.\n");
        hid_exit();
        return 1;
    }
    printf("Found SMX device: %s\n", path.c_str());

    hid_device *readDev = hid_open_path(path.c_str());
    if(!readDev)
    {
        fprintf(stderr, "First open failed.\n");
        hid_exit();
        return 1;
    }
    printf("First open OK.\n");
    hid_set_nonblocking(readDev, 1);

    if(single)
    {
        printf("Mode: SINGLE handle (shared behind a mutex, like the pre-split SDK).\n");
        std::mutex m;
        RunProbe(
            "single-handle (control)",
            [&](uint8_t *b, size_t l) { std::lock_guard<std::mutex> g(m); return hid_read(readDev, b, l); },
            [&](const uint8_t *b, size_t l) { std::lock_guard<std::mutex> g(m); return hid_write(readDev, b, l); });
        hid_close(readDev);
    }
    else
    {
        hid_device *writeDev = hid_open_path(path.c_str());
        if(!writeDev)
        {
            printf("SECOND open FAILED: double-open is not allowed here.\n");
            printf("=> The handle split would need a different approach on this platform.\n");
            hid_close(readDev);
            hid_exit();
            return 2;
        }
        printf("SECOND open OK (write handle). Double-open is allowed here.\n");
        printf("Mode: TWO handles (separate read/write, no shared lock).\n");
        RunProbe(
            "two-handle (split)",
            [&](uint8_t *b, size_t l) { return hid_read(readDev, b, l); },
            [&](const uint8_t *b, size_t l) { return hid_write(writeDev, b, l); });
        hid_close(readDev);
        hid_close(writeDev);
    }

    hid_exit();
    return 0;
}
