#pragma once
//
// EyeToyUsb — the EyeToy's own libusb context and event-pumping thread.
//
// Deliberately SEPARATE from the vendored PS3EYEDriver's libusb context
// (ps3eye.cpp owns its own libusb_init + transfer thread). Two reasons:
//   1. ps3eye.cpp calls libusb_init itself and owns that context plus its own
//      transfer thread, so there is no context to share. (It is a vendored
//      FORK, not a pristine drop — see third_party/ps3eye/README.md.)
//   2. Event-loop isolation: the EyeToy's iso completion handling never shares
//      a thread with the PS3 Eye's bulk path, so neither can stall the other.
//
// Process-lifetime singleton (magic-static). The libusb context is created once
// and lives for the process; the event thread runs only while >=1 device holds
// the context (ref-counted Acquire/Release), so an idle machine with no EyeToy
// streaming burns zero CPU here — same zero-idle discipline as the PS3 path.
//
#include <string>
#include <vector>

#include "../../common/UsbCore.h"

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace eyetoy {

// USB identity of the EyeToy camera interface (both PID revisions).
bool IsEyeToy(unsigned short vid, unsigned short pid);

// Stable "bus.p1.p2..." physical port-path string for a device. Same physical
// port yields the same string across replugs, so it is the slot key.
std::string PortPath(libusb_device* dev);

// Port paths of every EyeToy currently present on the shared context.
std::vector<std::string> EnumeratePortPaths();

// Open the EyeToy at `portPath` (or the first one found if empty). Caller owns
// the returned handle (libusb_close). Returns nullptr if not found / open fails.
libusb_device_handle* OpenByPortPath(const std::string& portPath);


// The EyeToy's libusb context (ctx #2): a distinct INSTANCE of the shared
// usbcore::Context, for event-thread isolation. See common/UsbCore.h.
struct UsbContext
{
    static usbcore::Context& Instance();
};

} // namespace eyetoy
