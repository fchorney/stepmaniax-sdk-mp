#include "SMXHIDInterface.h"

#include <hidapi/hidapi.h>

using namespace std;

namespace SMX {

// --- Real hidapi-backed IHIDDevice implementation ---

class HIDAPIDevice : public IHIDDevice
{
public:
    explicit HIDAPIDevice(hid_device *pDev) : m_pDev(pDev) {}
    ~HIDAPIDevice() override { Close(); }

    int Read(uint8_t *buf, size_t len) override
    {
        return hid_read(m_pDev, buf, len);
    }

    int Write(const uint8_t *buf, size_t len) override
    {
        return hid_write(m_pDev, buf, len);
    }

    void Close() override
    {
        if(m_pDev)
        {
            hid_close(m_pDev);
            m_pDev = nullptr;
        }
    }

private:
    hid_device *m_pDev;
};

// --- Real hidapi-backed IHIDEnumerator implementation ---

class HIDAPIEnumerator : public IHIDEnumerator
{
public:
    void Init() override { hid_init(); }
    void Exit() override { hid_exit(); }

    vector<HIDDeviceInfo> Enumerate(uint16_t vid, uint16_t pid) override
    {
        vector<HIDDeviceInfo> results;
        hid_device_info *devs = hid_enumerate(vid, pid);
        for(const hid_device_info *cur = devs; cur; cur = cur->next)
        {
            HIDDeviceInfo info;
            if(cur->path)
                info.sPath = cur->path;
            if(cur->product_string)
                info.sProduct = cur->product_string;
            results.push_back(std::move(info));
        }
        hid_free_enumeration(devs);
        return results;
    }

    unique_ptr<IHIDDevice> Open(const string &path) override
    {
        hid_device *pDev = hid_open_path(path.c_str());
        if(!pDev)
            return nullptr;
        hid_set_nonblocking(pDev, 1);
        return unique_ptr<IHIDDevice>(new HIDAPIDevice(pDev));
    }
};

unique_ptr<IHIDEnumerator> CreateHIDAPIEnumerator()
{
    return unique_ptr<IHIDEnumerator>(new HIDAPIEnumerator());
}

} // namespace SMX
