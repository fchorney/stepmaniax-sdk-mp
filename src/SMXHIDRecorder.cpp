#include "SMXHIDRecorder.h"

#include <algorithm>
#include <cstring>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#endif

using namespace std;

namespace SMX {

// Create directory and parents (like mkdir -p).
static void CreateDirectoryRecursive(const string &sPath)
{
    for(size_t i = 1; i < sPath.size(); i++)
    {
        if(sPath[i] == '/' || sPath[i] == '\\')
        {
            string sDir = sPath.substr(0, i);
#ifdef _WIN32
            _mkdir(sDir.c_str());
#else
            mkdir(sDir.c_str(), 0755);
#endif
        }
    }
#ifdef _WIN32
    _mkdir(sPath.c_str());
#else
    mkdir(sPath.c_str(), 0755);
#endif
}

// --- RecordingHIDDevice ---

RecordingHIDDevice::RecordingHIDDevice(unique_ptr<IHIDDevice> pDevice, const string &sOutputPath)
    : m_pDevice(std::move(pDevice))
    , m_File(sOutputPath, ios::binary)
    , m_StartTime(chrono::steady_clock::now())
{
    m_File.write(HID_CAPTURE_MAGIC, HID_CAPTURE_MAGIC_SIZE);
}

RecordingHIDDevice::~RecordingHIDDevice()
{
    Close();
}

int RecordingHIDDevice::Read(uint8_t *buf, size_t len)
{
    int iResult = m_pDevice->Read(buf, len);
    if(iResult > 0)
        WriteRecord('R', buf, static_cast<size_t>(iResult));
    return iResult;
}

int RecordingHIDDevice::Write(const uint8_t *buf, size_t len)
{
    int iResult = m_pDevice->Write(buf, len);
    if(iResult > 0)
        WriteRecord('W', buf, len);
    return iResult;
}

void RecordingHIDDevice::Close()
{
    if(m_pDevice)
    {
        m_pDevice->Close();
        m_pDevice.reset();
    }
    if(m_File.is_open())
        m_File.close();
}

void RecordingHIDDevice::WriteRecord(char cType, const uint8_t *buf, size_t len)
{
    lock_guard<mutex> lock(m_Mutex);
    auto now = chrono::steady_clock::now();
    uint64_t iUs = static_cast<uint64_t>(
        chrono::duration_cast<chrono::microseconds>(now - m_StartTime).count());
    uint16_t iSize = static_cast<uint16_t>(len);

    m_File.write(&cType, 1);
    m_File.write(reinterpret_cast<const char *>(&iUs), 8);
    m_File.write(reinterpret_cast<const char *>(&iSize), 2);
    m_File.write(reinterpret_cast<const char *>(buf), len);
    m_File.flush();
}

// --- RecordingHIDEnumerator ---

RecordingHIDEnumerator::RecordingHIDEnumerator(unique_ptr<IHIDEnumerator> pEnumerator, const string &sOutputDir)
    : m_pEnumerator(std::move(pEnumerator))
    , m_sOutputDir(sOutputDir)
{
    CreateDirectoryRecursive(sOutputDir);
}

void RecordingHIDEnumerator::Init() { m_pEnumerator->Init(); }
void RecordingHIDEnumerator::Exit() { m_pEnumerator->Exit(); }

vector<HIDDeviceInfo> RecordingHIDEnumerator::Enumerate(uint16_t vid, uint16_t pid)
{
    return m_pEnumerator->Enumerate(vid, pid);
}

unique_ptr<IHIDDevice> RecordingHIDEnumerator::Open(const string &path)
{
    auto pDevice = m_pEnumerator->Open(path);
    if(!pDevice)
        return nullptr;

    string sFile = m_sOutputDir + "/device_" + to_string(m_iDeviceCount++) + ".smxhid";
    return unique_ptr<IHIDDevice>(new RecordingHIDDevice(std::move(pDevice), sFile));
}

// --- ReplayHIDDevice ---

ReplayHIDDevice::ReplayHIDDevice(const string &sInputPath)
{
    auto records = LoadHIDCapture(sInputPath);

    // Group reads into batches separated by writes.
    // Batch 0 = reads before first write, batch 1 = reads after first write, etc.
    m_aReadBatches.emplace_back();  // batch 0
    for(auto &rec : records)
    {
        if(rec.cType == 'R')
            m_aReadBatches.back().push(std::move(rec.aData));
        else if(rec.cType == 'W')
        {
            m_aExpectedWrites.push_back(std::move(rec.aData));
            m_aReadBatches.emplace_back();  // start new batch
        }
    }
}

int ReplayHIDDevice::Read(uint8_t *buf, size_t len)
{
    lock_guard<mutex> lock(m_Mutex);

    // Return reads from all batches up to and including the current write count.
    for(int i = 0; i <= m_iWriteCount && i < static_cast<int>(m_aReadBatches.size()); i++)
    {
        if(!m_aReadBatches[i].empty())
        {
            auto &pkt = m_aReadBatches[i].front();
            size_t n = min(len, pkt.size());
            memcpy(buf, pkt.data(), n);
            m_aReadBatches[i].pop();
            return static_cast<int>(n);
        }
    }
    return 0;
}

int ReplayHIDDevice::Write(const uint8_t *buf, size_t len)
{
    lock_guard<mutex> lock(m_Mutex);
    m_aActualWrites.emplace_back(buf, buf + len);
    m_iWriteCount++;
    return static_cast<int>(len);
}

void ReplayHIDDevice::Close() {}

// --- LoadHIDCapture ---

vector<HIDCaptureRecord> LoadHIDCapture(const string &sPath)
{
    vector<HIDCaptureRecord> records;
    ifstream file(sPath, ios::binary);
    if(!file.is_open())
        return records;

    // Verify magic
    char magic[HID_CAPTURE_MAGIC_SIZE];
    file.read(magic, HID_CAPTURE_MAGIC_SIZE);
    if(memcmp(magic, HID_CAPTURE_MAGIC, HID_CAPTURE_MAGIC_SIZE) != 0)
        return records;

    while(file.good())
    {
        HIDCaptureRecord rec;
        file.read(&rec.cType, 1);
        if(!file.good())
            break;
        file.read(reinterpret_cast<char *>(&rec.iTimestampUs), 8);
        uint16_t iSize = 0;
        file.read(reinterpret_cast<char *>(&iSize), 2);
        if(!file.good())
            break;
        rec.aData.resize(iSize);
        file.read(reinterpret_cast<char *>(rec.aData.data()), iSize);
        if(!file.good() && !file.eof())
            break;
        records.push_back(std::move(rec));
    }
    return records;
}

} // namespace SMX
