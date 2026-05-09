#ifndef SMXHIDRecorder_h
#define SMXHIDRecorder_h

#include "SMXHIDInterface.h"

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

namespace SMX {

// --- HID traffic capture file format ---
//
// File header: "SMXHID\x01" (magic + version 1)
// Records: [type:1][timestamp_us:8][size:2][data:size]
//
// type: 'R' = read, 'W' = write
// timestamp_us: microseconds since recording start (little-endian)
// size: payload length (little-endian uint16)
// data: raw HID packet bytes

struct HIDCaptureRecord
{
    char cType;              // 'R' or 'W'
    uint64_t iTimestampUs;   // microseconds since start
    std::vector<uint8_t> aData;
};

// --- RecordingHIDDevice: wraps a real device and logs traffic ---

class RecordingHIDDevice : public IHIDDevice
{
public:
    RecordingHIDDevice(std::unique_ptr<IHIDDevice> pDevice, const std::string &sOutputPath);
    ~RecordingHIDDevice() override;

    int Read(uint8_t *buf, size_t len) override;
    int Write(const uint8_t *buf, size_t len) override;
    void Close() override;

private:
    void WriteRecord(char cType, const uint8_t *buf, size_t len);

    std::unique_ptr<IHIDDevice> m_pDevice;
    std::ofstream m_File;
    std::mutex m_Mutex;
    std::chrono::steady_clock::time_point m_StartTime;
};

// --- RecordingHIDEnumerator: wraps a real enumerator and records all opened devices ---

class RecordingHIDEnumerator : public IHIDEnumerator
{
public:
    RecordingHIDEnumerator(std::unique_ptr<IHIDEnumerator> pEnumerator, const std::string &sOutputDir);

    void Init() override;
    void Exit() override;
    std::vector<HIDDeviceInfo> Enumerate(uint16_t vid, uint16_t pid) override;
    std::unique_ptr<IHIDDevice> Open(const std::string &path) override;

private:
    std::unique_ptr<IHIDEnumerator> m_pEnumerator;
    std::string m_sOutputDir;
    int m_iDeviceCount = 0;
};

// --- ReplayHIDDevice: replays captured traffic for regression tests ---
//
// Reads are gated by writes to preserve the original request-response ordering.
// Reads recorded before the first write are available immediately. Reads between
// write N and write N+1 become available only after the Nth Write() call is made.

class ReplayHIDDevice : public IHIDDevice
{
public:
    explicit ReplayHIDDevice(const std::string &sInputPath);

    int Read(uint8_t *buf, size_t len) override;
    int Write(const uint8_t *buf, size_t len) override;
    void Close() override;

    const std::vector<std::vector<uint8_t>> &GetExpectedWrites() const { return m_aExpectedWrites; }
    const std::vector<std::vector<uint8_t>> &GetActualWrites() const { return m_aActualWrites; }

private:
    // Read batches: m_aReadBatches[i] contains reads available after i writes have occurred.
    std::vector<std::queue<std::vector<uint8_t>>> m_aReadBatches;
    int m_iWriteCount = 0;
    std::vector<std::vector<uint8_t>> m_aExpectedWrites;
    std::vector<std::vector<uint8_t>> m_aActualWrites;
    std::mutex m_Mutex;
};

// --- Utility: load all records from a capture file ---

std::vector<HIDCaptureRecord> LoadHIDCapture(const std::string &sPath);

} // namespace SMX

#endif
