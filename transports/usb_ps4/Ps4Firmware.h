#pragma once
//
// Ps4Firmware — locate, verify, and upload the PS4 camera's OV580 firmware.
//
// The PS4 camera does nothing until a proprietary Sony firmware blob is pushed
// over USB to the 05A9:0580 bootloader; it then re-enumerates as 05A9:058A and
// streams via the OV580's internal ISP (UVC probe/commit).
//
// This repo ships NO Sony blobs (they are Sony IP). The app LOCATES them in a
// cache directory and HASH-VERIFIES the firmware against a known-good table
// before ever sending it to hardware, so a wrong/corrupt file can't be uploaded:
//
//   1. %ProgramData%\PSCam4Win\firmware.bin  + startup.bin   (production cache)
//   2. <exe-dir>\firmware.bin                + startup.bin   (portable install)
//   3. <cwd>\build\firmware.bin              + startup.bin   (developer build dir)
//
// ONE firmware build is supported (see kKnownFirmware): the FINAL OV580 build,
// byte-identical across PS4 system 6.00–7.02 — the last one Sony ever shipped.
// Adopted as the sole build after hardware A/B against the older ps4eye-era
// blob: identical streaming and controls, and it FIXES the close→reopen wedge
// (no replug after a tray restart). The setup exe auto-downloads it into
// cache (1), SHA-1-pinned (installer/InstallSteps.cpp StageFirmwareStep), and
// replaces any outdated cached copy; all other bytes are refused here. After
// upload the device re-enumerates as the COMPOSITE 05A9:058B (IAD video
// function, WinUSB on the MI_00 child) — see Ps4Usb.h. (058A, the identity an
// OLD build presents, stays openable so a camera still running a pre-upgrade
// upload keeps working until its next replug.)
//
#include <cstdint>
#include <string>
#include <vector>

struct libusb_device_handle;

namespace ps4 {

// A known-good OV580 firmware build (SHA-1 over the whole file).
struct FirmwareInfo
{
    const char*    sha1;   // lowercase hex, 40 chars
    uint32_t       size;   // exact byte size (informational)
    const wchar_t* name;   // human-readable build name for logs
};

// The single supported build: the final OV580 firmware (PS4 sys 6.00–7.02,
// byte-identical across those system versions — verified against
// psxdev/luke_firmwares 600/650/700/702, OrbisEyeCam, and Hackinside copies).
extern const FirmwareInfo kKnownFirmware[1];
constexpr int kFirmwareCount = 1;

struct FirmwareBlobs
{
    std::vector<uint8_t> firmware;   // SHA-1-verified OV580 firmware
    std::vector<uint8_t> startup;    // startup control-transfer replay script
    int firmwareIndex = -1;          // index into kKnownFirmware (-1 = none)
    bool Ok() const { return !firmware.empty() && !startup.empty(); }
};

// Locate firmware.bin + startup.bin (search order above) and verify the
// firmware against kKnownFirmware. Returns blobs; `firmware` is left empty
// when the file is missing or does not hash-match (so unknown bytes can never
// be uploaded), and `firmwareIndex` says which build matched.
FirmwareBlobs EnsureFirmware();

// Upload `fw` to a boot-mode (0580) handle in 512-byte chunks via the proven
// control-transfer sequence, then issue the reboot command that re-enumerates
// the device in streaming mode. Returns true if every chunk was sent (the reboot
// transfer itself tears the device down and is expected to error). The OV580
// bootloader's chunk protocol is independent of the firmware build.
bool UploadFirmware(libusb_device_handle* bootHandle, const std::vector<uint8_t>& fw);

// SHA-1 of `data` as a lowercase hex string ("" on failure). Exposed for tests.
std::string Sha1Hex(const std::vector<uint8_t>& data);

// Index into kKnownFirmware for these bytes, or -1 if not a known-good build.
int IdentifyFirmware(const std::vector<uint8_t>& data);

} // namespace ps4
