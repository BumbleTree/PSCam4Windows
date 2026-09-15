#include "UsbCore.h"

#include <libusb.h>
#include <cstdio>

namespace usbcore {

std::string PortPath(libusb_device* dev)
{
    uint8_t ports[8];
    const int n = libusb_get_port_numbers(dev, ports, sizeof(ports));
    char buf[64];
    int off = snprintf(buf, sizeof(buf), "%d", libusb_get_bus_number(dev));
    for (int i = 0; i < n && off > 0 && off < (int)sizeof(buf); ++i)
        off += snprintf(buf + off, sizeof(buf) - off, ".%d", ports[i]);
    return std::string(buf);
}

namespace {

// Walk the device list, freeing it on every path out. Returning true from
// `visit` stops the walk.
template <typename Visit>
void ForEachDevice(libusb_context* ctx, Visit&& visit)
{
    if (!ctx)
        return;
    libusb_device** list = nullptr;
    const ssize_t cnt = libusb_get_device_list(ctx, &list);
    for (ssize_t i = 0; i < cnt; ++i)
    {
        libusb_device_descriptor dd{};
        if (libusb_get_device_descriptor(list[i], &dd) != 0)
            continue;
        if (visit(list[i], dd))
            break;
    }
    if (list)
        libusb_free_device_list(list, 1);
}

libusb_device_handle* OpenFirstMatching(libusb_context* ctx,
                                        const std::string& portPath,
                                        MatchFn match,
                                        unsigned short vid, unsigned short pid,
                                        bool byVidPid)
{
    libusb_device_handle* h = nullptr;
    ForEachDevice(ctx, [&](libusb_device* dev, const libusb_device_descriptor& dd) {
        const bool ids = byVidPid ? (dd.idVendor == vid && dd.idProduct == pid)
                                  : (match && match(dd.idVendor, dd.idProduct));
        if (!ids)
            return false;
        if (!portPath.empty() && PortPath(dev) != portPath)
            return false;
        if (libusb_open(dev, &h) != 0)
            h = nullptr;
        return true;   // first match wins, open succeeded or not
    });
    return h;
}

} // namespace

std::vector<std::string> EnumeratePortPaths(libusb_context* ctx, MatchFn match)
{
    std::vector<std::string> paths;
    ForEachDevice(ctx, [&](libusb_device* dev, const libusb_device_descriptor& dd) {
        if (match && match(dd.idVendor, dd.idProduct))
            paths.push_back(PortPath(dev));
        return false;   // visit them all
    });
    return paths;
}

libusb_device_handle* OpenByPortPath(libusb_context* ctx, MatchFn match,
                                     const std::string& portPath)
{
    return OpenFirstMatching(ctx, portPath, match, 0, 0, false);
}

libusb_device_handle* OpenByVidPid(libusb_context* ctx,
                                   unsigned short vid, unsigned short pid,
                                   const std::string& portPath)
{
    return OpenFirstMatching(ctx, portPath, nullptr, vid, pid, true);
}

// ---------------------------------------------------------------- Context ----

Context::Context()
{
    libusb_init(&_ctx);
}

Context::~Context()
{
    // Best-effort: if a device leaked a reference the thread is still running.
    if (_thread.joinable())
    {
        _exit.store(true, std::memory_order_release);
        _thread.join();
    }
    if (_ctx)
        libusb_exit(_ctx);
}

void Context::Acquire()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_users++ == 0)
    {
        _exit.store(false, std::memory_order_release);
        _thread = std::thread(&Context::EventThreadFunc, this);
    }
}

void Context::Release()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_users > 0 && --_users == 0)
    {
        _exit.store(true, std::memory_order_release);
        if (_thread.joinable())
            _thread.join();
    }
}

void Context::EventThreadFunc()
{
    // Pump libusb events on a 50 ms timeout (the same cadence as the vendored
    // ps3eye transfer thread). The timeout bounds shutdown latency without
    // busy-spinning; isochronous completion callbacks fire from here.
    while (!_exit.load(std::memory_order_acquire))
    {
        timeval tv{ 0, 50 * 1000 };
        libusb_handle_events_timeout_completed(_ctx, &tv, nullptr);
    }
}

} // namespace usbcore
