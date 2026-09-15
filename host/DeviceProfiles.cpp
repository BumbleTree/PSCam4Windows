#include "DeviceProfiles.h"

// PS3 Eye: all 13 native modes (Settings.h kVideoModes); serves YUY2 + NV12;
// default NV12 640x480@60 (unchanged from the current DLL default). No LED hook
// (the PS3 Eye LED follows USB power, not a software GPIO).
static const DeviceProfile kPs3EyeProfile = {
    0x1415, 0x2000,
    TransportClass::UsbBulk_Libusb,
    L"PS3 Eye",
    kVideoModes, static_cast<uint32_t>(kVideoModeCount),
    FMT_YUY2 | FMT_NV12,
    { 640, 480, 60 },
    FMT_NV12,
    false,
    // Ps3EyeDevice backs flip, auto+manual gain/exposure, AWB + manual R/G/B,
    // and the test pattern. It does NOT wire brightness/saturation (greyed).
    CTRL_FLIP | CTRL_GAIN | CTRL_EXPOSURE | CTRL_WHITEBAL | CTRL_WB_MANUAL | CTRL_TESTPATTERN,
};

const DeviceProfile& Ps3EyeProfile() { return kPs3EyeProfile; }

// PS2 EyeToy (OV519 bridge + OV7648). Streams JFIF over WinUSB iso; serves YUY2
// (decoded) + MJPEG (passthrough). Default YUY2 320x240@30 — the PCSX2-verified
// path. Software LED (OV519 GPIO 0x71).
static const VideoMode kEyeToyModes[] = {
    { 320, 240, 30 },   // default
    { 320, 240, 15 },
    { 640, 480, 15 },
};
static const DeviceProfile kEyeToyProfile = {
    0x054C, 0x0154,
    TransportClass::UsbIso_Libusb,
    L"PS2 EyeToy",
    kEyeToyModes, static_cast<uint32_t>(sizeof(kEyeToyModes) / sizeof(kEyeToyModes[0])),
    FMT_YUY2 | FMT_MJPEG,
    { 320, 240, 30 },
    FMT_YUY2,
    true,
    // OV7648 controls: brightness (I2C 0x06) and saturation (0x03) via the
    // proven gspca ov519 path, plus FLIP done in software during the YUY2 repack
    // (the OV7648 has no hardware flip register; EyeToyDevice mirrors the decoded
    // frame on the CPU — negligible at QVGA/VGA). Gain/exposure stay on the
    // sensor's auto defaults (gspca exposes neither for the OV7648). The AWB
    // toggle is omitted on purpose: the OV7648 has no manual R/G/B gain
    // registers, so toggling AWB off would unlock nothing (only freeze white
    // balance with no correction). AWB is left permanently on (bring-up default).
    CTRL_FLIP | CTRL_BRIGHTNESS | CTRL_SATURATION,
};

const DeviceProfile& EyeToyProfile() { return kEyeToyProfile; }

// PS4 Camera (OmniVision OV580 bridge + dual OV9713 sensors). After firmware
// upload it streams calibrated-colour YUY2 from the OV580's internal ISP over
// WinUSB iso (both eyes in every frame); Ps4ViewDevice copies/stitches the
// per-eye bands. Each logical view is its own virtual camera with its own
// resolution: a single eye is the sensor-native 1280x800, SideBySide stitches
// both into 2560x800. NINE delivered modes each, 1280x800@60/30/15 down to
// 320x192@240 — the tables below mirror ps4::kIspGeometries, which is the
// hardware-verified source of truth. Serves YUY2 (canonical) + NV12 (DLL
// downsample); no MJPEG.
//
// Controls, measured per-eye on the shipped firmware with the control pipe
// healthy — a stalled pipe swallows writes and makes every control look inert,
// so any re-measurement has to clear the halt first:
//   PU_BRIGHTNESS 0..8  linear luma offset ~15/step — but it CLIPS: 3.7% of the
//                       frame is pinned white at 4, 18% at 8.
//   PU_CONTRAST   0..8  a real tone curve (mean 63 -> 179 across the range).
//   PU_GAIN       0..8  mean 100 -> 148 with NO clipping at either end — the
//                       right knob for taming the ISP's high AE target, which is
//                       what makes this camera look blown out next to a PS3 Eye.
//   PU_WB_TEMP    2800..6500 K plus PU_WB_TEMP_AUTO — real white balance.
// Manual EXPOSURE remains impossible (both AE modes the OV580 accepts are auto,
// re-tested on a healthy pipe: a low exposure made the image BRIGHTER), so
// CTRL_EXPOSURE and CTRL_GAIN's auto toggle stay off; gain is advertised as
// CTRL_ISPGAIN, which is a plain always-live slider. SATURATION and SHARPNESS
// are live and ARE advertised (saturation 0 gives U=V=128 exactly, 8 doubles
// chroma magnitude; sharpness 0->8 moves mean |dY/dx| 3.05 -> 5.14). Both look
// inert to luma-only photometry, which is why they must be measured in chroma.
// HUE is the one that stays hidden — genuinely inert.
// All nine hardware-verified delivered modes (see ps4::kIspGeometries).
// 120 and 240 fps DO work: the old "60 fps only" verdict
// came from sending the 1280x800 dwMaxVideoFrameSize with every frame index.
static const VideoMode kPs4EyeModes[] = {
    { 1280, 800,  60 },   // default — full stereo, sensor-native
    { 1280, 800,  30 },
    { 1280, 800,  15 },
    {  640, 400, 120 },   // high-speed: half-height readout
    {  640, 400,  60 },
    {  640, 400,  30 },
    {  320, 192, 240 },   // very high-speed: tracking / light-gun work
    {  320, 192, 120 },
    {  320, 192,  60 },
};
// Side-by-side is two eyes wide at the same per-eye geometry.
static const VideoMode kPs4SbsModes[] = {
    { 2560, 800,  60 },   // default
    { 2560, 800,  30 },
    { 2560, 800,  15 },
    { 1280, 400, 120 },
    { 1280, 400,  60 },
    { 1280, 400,  30 },
    {  640, 192, 240 },
    {  640, 192, 120 },
    {  640, 192,  60 },
};
static const uint32_t kPs4EyeModeCount = (uint32_t)(sizeof(kPs4EyeModes) / sizeof(kPs4EyeModes[0]));
static const uint32_t kPs4SbsModeCount = (uint32_t)(sizeof(kPs4SbsModes) / sizeof(kPs4SbsModes[0]));

// The ISP's picture controls are 0..8, so their sliders get nine detents rather
// than 256 positions for nine outcomes. White balance is a real Kelvin axis.
static const ControlRange kPs4Ranges[] = {
    { CTRL_BRIGHTNESS, 9, 0, 0 },
    { CTRL_CONTRAST,   9, 0, 0 },
    { CTRL_ISPGAIN,    9, 0, 0 },
    { CTRL_SATURATION, 9, 0, 0 },
    { CTRL_SHARPNESS,  9, 0, 0 },
    { CTRL_WB_TEMP,    0, 2800, 6500 },
};
static const uint32_t kPs4RangeCount = (uint32_t)(sizeof(kPs4Ranges) / sizeof(kPs4Ranges[0]));

static const DeviceProfile kPs4LeftProfile = {
    0x05A9, 0x058A,
    TransportClass::Usb_Ps4,
    L"PS4 Camera (Left)",
    kPs4EyeModes, kPs4EyeModeCount,
    FMT_YUY2 | FMT_NV12,
    { 1280, 800, 60 },
    FMT_YUY2,
    false,
    CTRL_FLIP | CTRL_BRIGHTNESS | CTRL_CONTRAST | CTRL_ISPGAIN | CTRL_WHITEBAL |
        CTRL_WB_TEMP | CTRL_MICARRAY | CTRL_SATURATION | CTRL_SHARPNESS |
        CTRL_POWERLINE,
    kPs4Ranges, kPs4RangeCount,
};
static const DeviceProfile kPs4RightProfile = {
    0x05A9, 0x058A,
    TransportClass::Usb_Ps4,
    L"PS4 Camera (Right)",
    kPs4EyeModes, kPs4EyeModeCount,
    FMT_YUY2 | FMT_NV12,
    { 1280, 800, 60 },
    FMT_YUY2,
    false,
    CTRL_FLIP | CTRL_BRIGHTNESS | CTRL_CONTRAST | CTRL_ISPGAIN | CTRL_WHITEBAL |
        CTRL_WB_TEMP | CTRL_MICARRAY | CTRL_SATURATION | CTRL_SHARPNESS |
        CTRL_POWERLINE,
    kPs4Ranges, kPs4RangeCount,
};
static const DeviceProfile kPs4SbsProfile = {
    0x05A9, 0x058A,
    TransportClass::Usb_Ps4,
    L"PS4 Camera",
    kPs4SbsModes, kPs4SbsModeCount,
    FMT_YUY2 | FMT_NV12,
    { 2560, 800, 60 },
    FMT_YUY2,
    false,
    CTRL_FLIP | CTRL_BRIGHTNESS | CTRL_CONTRAST | CTRL_ISPGAIN | CTRL_WHITEBAL |
        CTRL_WB_TEMP | CTRL_MICARRAY | CTRL_SATURATION | CTRL_SHARPNESS |
        CTRL_POWERLINE,
    kPs4Ranges, kPs4RangeCount,
};

const DeviceProfile& Ps4ViewProfile(Ps4ViewKind view)
{
    switch (view)
    {
    case Ps4ViewKind::Left:  return kPs4LeftProfile;
    case Ps4ViewKind::Right: return kPs4RightProfile;
    default:                 return kPs4SbsProfile;   // SideBySide
    }
}

const DeviceProfile* FindDeviceProfile(uint16_t vid, uint16_t pid)
{
    if (vid == 0x1415 && pid == 0x2000) return &kPs3EyeProfile;
    if (vid == 0x054C && (pid == 0x0154 || pid == 0x0155)) return &kEyeToyProfile;
    // PS4 camera in ANY USB identity (0580 bootloader / 058A legacy-firmware
    // streaming / 058B final-firmware composite streaming) resolves to the base
    // "PS4 Camera" profile. The registry picks the actual per-slot view
    // (Left/Right/SBS) via Ps4ViewProfile + Settings; this is the physical-ID lookup.
    if (vid == 0x05A9 && (pid == 0x0580 || pid == 0x058A || pid == 0x058B)) return &kPs4SbsProfile;
    return nullptr;
}
