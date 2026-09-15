#include "EyeToyUsb.h"

#include "../../common/UsbCore.h"

// Facade over usbcore (common/UsbCore.h): the EyeToy VID/PIDs, and ctx #2.

namespace eyetoy {

bool IsEyeToy(unsigned short vid, unsigned short pid)
{
    return vid == 0x054C && (pid == 0x0154 || pid == 0x0155);
}

std::string PortPath(libusb_device* dev)
{
    return usbcore::PortPath(dev);
}

std::vector<std::string> EnumeratePortPaths()
{
    return usbcore::EnumeratePortPaths(UsbContext::Instance().Get(), &IsEyeToy);
}

libusb_device_handle* OpenByPortPath(const std::string& portPath)
{
    return usbcore::OpenByPortPath(UsbContext::Instance().Get(), &IsEyeToy, portPath);
}

// ctx #2. Distinct instance from the PS4's by design -- see common/UsbCore.h.
usbcore::Context& UsbContext::Instance()
{
    static usbcore::Context sInstance;   // thread-safe magic static; process lifetime
    return sInstance;
}

} // namespace eyetoy
