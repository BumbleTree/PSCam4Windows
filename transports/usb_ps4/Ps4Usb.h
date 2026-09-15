#pragma once
//
// Ps4Usb — the PS4 camera's own libusb context (the THIRD in the process) and
// event-pumping thread.
//
// Separate from the PS3 driver's context (ctx #1, which that vendored fork
// creates and owns itself — see third_party/ps3eye/README.md) and
// the EyeToy's context (ctx #2) for the same two reasons as the EyeToy split,
// plus one more that matters here: the PS4 camera streams raw-Bayer stereo at
// ~275 MB/s on SuperSpeed iso, so its completion handling must never share a
// thread with the lighter EyeToy/PS3 paths or it could starve them (and vice
// versa). One context, one event thread, ref-counted Acquire/Release so an idle
// machine with no PS4 streaming burns zero CPU here.
//
// The PS4 camera lives in THREE USB identities: 05A9:0580 (OV580 bootloader,
// "USB Boot") before firmware; then, depending on which firmware build was
// uploaded, 05A9:058A ("USB Camera-OV580", flat device — legacy ps4eye blob)
// or 05A9:058B (same name but a COMPOSITE device with an IAD video function,
// WinUSB-bound at MI_00 — the final PS4-sys-6.00+ build). It re-enumerates AT
// THE SAME PHYSICAL PORT PATH during firmware upload, so the port path — not
// the VID/PID — is the stable slot key, and IsPs4()/enumeration deliberately
// match all three PIDs.
//
#include <string>
#include <vector>

#include "../../common/UsbCore.h"

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace ps4 {

constexpr unsigned short kVid       = 0x05A9;   // OmniVision (PS4 camera OV580 bridge)
constexpr unsigned short kPidBoot   = 0x0580;   // bootloader, pre-firmware
constexpr unsigned short kPidRun    = 0x058A;   // streaming, legacy firmware build
constexpr unsigned short kPidRunNew = 0x058B;   // streaming, final (6.00+) firmware build

// Matches the PS4 camera in ANY state (boot or either streaming identity).
bool IsPs4(unsigned short vid, unsigned short pid);
bool IsPs4Boot(unsigned short vid, unsigned short pid);
bool IsPs4Run(unsigned short vid, unsigned short pid);

// Stable "bus.p1.p2..." physical port-path string. The same physical port
// yields the same string across replugs AND across the 0580->058A firmware
// re-enumeration, so it is the slot key.
std::string PortPath(libusb_device* dev);

// Port paths of every PS4 camera currently present (boot or streaming), so a
// camera mid-firmware-upload still appears at its stable path.
std::vector<std::string> EnumeratePortPaths();

// Open the PS4 camera at `portPath` (empty = first found) that matches `pid`
// (kPidBoot / kPidRun / kPidRunNew). Caller owns the returned handle
// (libusb_close). Returns nullptr if not present / open fails (e.g. WinUSB not
// yet bound).
libusb_device_handle* OpenByPortPath(const std::string& portPath, unsigned short pid);

// Open the STREAMING device at `portPath` whichever firmware build produced it
// (tries 058A, then 058B). The two identities expose the same VC/VS interfaces
// (0/1) and iso EP 0x81, so callers drive them identically.
libusb_device_handle* OpenRunning(const std::string& portPath);

// The PS4's libusb context (ctx #3): a distinct INSTANCE of the shared
// usbcore::Context, for event-thread isolation. See common/UsbCore.h.
struct UsbContext
{
    static usbcore::Context& Instance();
};

} // namespace ps4
