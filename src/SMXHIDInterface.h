#ifndef SMXHIDInterface_h
#define SMXHIDInterface_h

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace SMX {

/// Abstract interface for a single HID device connection.
/// Production code uses HIDAPIDevice; tests inject a fake.
class IHIDDevice
{
public:
    virtual ~IHIDDevice() = default;
    virtual int Read(uint8_t *buf, size_t len) = 0;
    virtual int Write(const uint8_t *buf, size_t len) = 0;
    virtual void Close() = 0;
};

/// Information about a discovered HID device.
struct HIDDeviceInfo
{
    std::string sPath;
    std::wstring sProduct;
};

/// Abstract interface for HID device enumeration and opening.
/// Production code uses HIDAPIEnumerator; tests inject a fake.
class IHIDEnumerator
{
public:
    virtual ~IHIDEnumerator() = default;
    virtual void Init() = 0;
    virtual void Exit() = 0;
    virtual std::vector<HIDDeviceInfo> Enumerate(uint16_t vid, uint16_t pid) = 0;
    virtual std::unique_ptr<IHIDDevice> Open(const std::string &path) = 0;
};

/// Creates the real hidapi-backed enumerator.
std::unique_ptr<IHIDEnumerator> CreateHIDAPIEnumerator();

} // namespace SMX

#endif
