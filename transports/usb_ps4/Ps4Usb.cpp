#include "Ps4Usb.h"

#include "../../common/UsbCore.h"

// Facade over usbcore (common/UsbCore.h): the three PS4 identities, and ctx #3.

namespace ps4 {

bool IsPs4(unsigned short vid, unsigned short pid)
{
    return vid == kVid && (pid == kPidBoot || pid == kPidRun || pid == kPidRunNew);
}
bool IsPs4Boot(unsigned short vid, unsigned short pid) { return vid == kVid && pid == kPidBoot; }
bool IsPs4Run(unsigned short vid, unsigned short pid)
{
    return vid == kVid && (pid == kPidRun || pid == kPidRunNew);
}

std::string PortPath(libusb_device* dev)
{
    return usbcore::PortPath(dev);
}

std::vector<std::string> EnumeratePortPaths()
{
    return usbcore::EnumeratePortPaths(UsbContext::Instance().Get(), &IsPs4);
}

// By exact pid: callers need one specific identity, never "whichever".
libusb_device_handle* OpenByPortPath(const std::string& portPath, unsigned short pid)
{
    return usbcore::OpenByVidPid(UsbContext::Instance().Get(), kVid, pid, portPath);
}

libusb_device_handle* OpenRunning(const std::string& portPath)
{
    libusb_device_handle* h = OpenByPortPath(portPath, kPidRun);
    if (!h)
        h = OpenByPortPath(portPath, kPidRunNew);
    return h;
}

// ctx #3. Distinct instance from the EyeToy's by design -- see common/UsbCore.h.
usbcore::Context& UsbContext::Instance()
{
    static usbcore::Context sInstance;   // thread-safe magic static; process lifetime
    return sInstance;
}

} // namespace ps4
