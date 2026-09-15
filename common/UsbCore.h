#pragma once
//
// UsbCore — libusb context, event thread and device lookup, shared by the EyeToy
// and PS4 transports.
//
// The IMPLEMENTATION is shared; the context INSTANCES are not. Each transport
// constructs its own Context so the PS4's ~275 MB/s isochronous completion
// handling never shares an event thread with the lighter paths. (The PS3 Eye is
// a third context inside the vendored driver, which calls libusb_init itself.)
//
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace usbcore {

// Matches a device by its descriptor ids.
using MatchFn = bool (*)(unsigned short vid, unsigned short pid);

// Stable "bus.p1.p2..." port path. Survives replug, and the PS4's
// 0580 -> 058A/058B firmware re-enumeration, so it works as a slot key.
std::string PortPath(libusb_device* dev);

// Port paths of every currently present device `match` accepts.
std::vector<std::string> EnumeratePortPaths(libusb_context* ctx, MatchFn match);

// Open the device at `portPath` (empty = first match). Caller owns the handle.
// Null if absent, or if the open failed — usually WinUSB not bound yet.
libusb_device_handle* OpenByPortPath(libusb_context* ctx, MatchFn match,
                                     const std::string& portPath);

// Same, by exact VID/PID. The PS4 needs to target one specific identity
// (bootloader vs streaming), not merely "a PS4 camera".
libusb_device_handle* OpenByVidPid(libusb_context* ctx,
                                   unsigned short vid, unsigned short pid,
                                   const std::string& portPath);

// One libusb context plus its event-pumping thread. Construct as a
// function-local static. The thread runs only while >=1 device holds it.
class Context
{
public:
    Context();
    ~Context();
    Context(const Context&) = delete;
    Context& operator=(const Context&) = delete;

    libusb_context* Get() const { return _ctx; }

    void Acquire();   // first Acquire starts the event thread
    void Release();   // last Release stops and joins it

private:
    void EventThreadFunc();

    libusb_context*   _ctx = nullptr;
    std::thread       _thread;
    std::atomic<bool> _exit{ false };
    std::mutex        _mutex;   // serializes user-count transitions
    int               _users = 0;
};

} // namespace usbcore
