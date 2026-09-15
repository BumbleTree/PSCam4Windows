#pragma once
//
// ICameraDevice — the capture-side abstraction. Mirrors ICameraPreviewSource on
// the producer side: it hides USB, sensor bring-up, and (for JPEG cameras) the
// decode, behind a transport-agnostic interface. CaptureController stays the
// orchestrator (state machine, FrameBus, ControlBus, IMFVirtualCamera) and never
// switches on a product enum — it drives whatever ICameraDevice the
// DeviceRegistry hands it for a slot, parameterised by a static DeviceProfile.
//
// Three implementations ship:
//   Ps3EyeDevice   — wraps the vendored PS3EYEDriver (bulk Bayer -> fused YUY2)
//   EyeToyDevice   — WinUSB isochronous via libusb, JFIF -> YUY2 (libjpeg-turbo)
//   Ps4ViewDevice  — one logical view (Left / Right / SideBySide) over a shared
//                    Ps4CaptureSource; the only one that implements SetView,
//                    ActualMode and the optional microphone methods below.
//
#include <cstdint>
#include "../common/Settings.h"   // VideoMode, Settings, kVideoModes

// How a device's frames reach us. PS3 Eye, EyeToy and PS4 camera all ride
// libusb's WinUSB backend; kept distinct because their capture loops and
// per-frame work differ. USB3_Custom is reserved for a future camera.
//
// NOTE the PS4 correction: the original scaffold reserved `UVC_Inbox = 3`
// ("PS4 — inbox usbvideo.sys, no libusb"). That assumption was WRONG. The PS4
// camera enumerates in an OmniVision OV580 *bootloader* (05A9:0580), does
// nothing until a firmware blob is uploaded over USB, then re-enumerates as
// 05A9:058A and streams raw-Bayer stereo isochronously on EP 0x81 — driven by
// libusb exactly like the EyeToy, not by usbvideo.sys. Hence `Usb_Ps4`.
enum class TransportClass : uint32_t
{
    None           = 0,
    UsbBulk_Libusb = 1,   // PS3 Eye  — WinUSB bulk
    UsbIso_Libusb  = 2,   // EyeToy   — WinUSB isochronous (libusb 1.0.27)
    Usb_Ps4        = 3,   // PS4 Cam  — WinUSB iso, firmware-loaded OV580 (ctx #3)
    Usb3_Custom    = 4,   // PS5 (future)
};

// Which logical view of a PS4 camera's stereo output a slot presents. One PS4
// can be ONE switchable camera (a single view, user-selected) or be split into
// TWO simultaneous cameras (Left + Right). SideBySide stitches both eyes into
// one wide stereoscopic frame. Stored persistently as a uint32_t (Settings).
enum class Ps4ViewKind : uint32_t
{
    Left       = 0,
    Right      = 1,
    SideBySide = 2,
};

// Pixel-format bits, shared by DeviceProfile::formatMask (what a device can
// serve) and ControlBus consumerMask (what a client is consuming).
enum FormatBits : uint32_t
{
    FMT_YUY2  = 1u,
    FMT_NV12  = 2u,
    FMT_MJPEG = 4u,
};

// Which Settings-dialog sensor controls a device actually backs in its
// ApplySettings(). DeviceProfile::controlMask drives which controls the dialog
// enables; a device greys out everything it does not advertise here, so users
// only ever touch settings that take effect. Video-mode selection is
// always available and comes from DeviceProfile::modes, not this mask.
enum ControlBits : uint32_t
{
    CTRL_FLIP        = 1u << 0,   // horizontal / vertical flip
    CTRL_GAIN        = 1u << 1,   // auto gain/exposure toggle + manual gain
    CTRL_WHITEBAL    = 1u << 2,   // auto white balance toggle (AWB on/off)
    CTRL_TESTPATTERN = 1u << 3,
    CTRL_WB_MANUAL   = 1u << 4,   // manual R/G/B balance gains (when AWB off)
    CTRL_BRIGHTNESS  = 1u << 5,   // brightness slider (EyeToy OV7648 reg 0x06)
    CTRL_SATURATION  = 1u << 6,   // saturation slider (EyeToy OV7648 reg 0x03)
    CTRL_EXPOSURE    = 1u << 7,   // manual exposure-time slider (interlocked with
                                  // CTRL_GAIN's auto toggle); split from CTRL_GAIN
                                  // so a device whose exposure write is dead can
                                  // drop just the slider and keep gain
    CTRL_CONTRAST    = 1u << 8,   // contrast slider (PS4 ISP PU_CONTRAST) — a real
                                  // tone curve, the strongest highlight lever the
                                  // OV580 exposes
    CTRL_ISPGAIN     = 1u << 9,   // ALWAYS-ACTIVE gain trim, NOT interlocked with an
                                  // auto toggle. The PS4's ISP runs auto-exposure
                                  // permanently, and PU_GAIN rides on top of it, so
                                  // unlike CTRL_GAIN there is no "manual mode" to
                                  // enter first. Drives the same Settings::gain
                                  // value and the same dialog row as CTRL_GAIN.
    CTRL_SHARPNESS   = 1u << 12,  // PU_SHARPNESS 0..8. Measured on the PS4 ISP:
                                  // detail 3.05 -> 5.14 across the range (+68 %),
                                  // invisible to luma-only photometry, which is
                                  // why it looked dead until the right metric.
    CTRL_POWERLINE   = 1u << 13,  // mains anti-flicker 0/50/60 Hz. A correctness
                                  // control, not a preference: the ISP default is
                                  // 50 Hz, so 60 Hz regions band with no recourse.
    CTRL_MICARRAY    = 1u << 11,  // device has an in-band microphone array the
                                  // dialog should show meters + a recorder for.
                                  // CAPABILITY only: whether audio is flowing
                                  // right now is AudioChannels(), which is the
                                  // interlock that greys the block. Getting that
                                  // split wrong is what made the block invisible
                                  // -- it was hidden on "no audio yet", which is
                                  // always true at the instant a camera is
                                  // selected, so it never came back.
    CTRL_WB_TEMP     = 1u << 10,  // white-balance TEMPERATURE slider (single Kelvin
                                  // axis), as opposed to CTRL_WB_MANUAL's per-channel
                                  // R/G/B gains. Interlocked with the AWB toggle.
};

// What a control's stored 0..255 (or 0..63) range means on this device, for the
// Settings UI. A control with no entry is a continuous percentage.
//
//   steps > 0  the device takes that many distinct values, so the slider gets
//              that many detents and reads "N / steps-1"
//   steps == 0 continuous, labelled with uMin..uMax in its own unit
struct ControlRange
{
    uint32_t cap;        // which CTRL_* this describes
    uint32_t steps;
    int32_t  uMin;
    int32_t  uMax;
};

// Static, data-only description of a supported camera. New cameras add a profile
// (and an ICameraDevice subclass only if their transport isn't already covered).
struct DeviceProfile
{
    uint16_t vid;
    uint16_t pid;
    TransportClass transport;
    const wchar_t* displayName;     // branding, used at MFCreateVirtualCamera
    const VideoMode* modes;         // advertised modes
    uint32_t modeCount;
    uint32_t formatMask;            // FMT_* the DLL may advertise for this device
    VideoMode defaultMode;
    uint32_t defaultFormat;         // FMT_NV12 (PS3 Eye) or FMT_YUY2 (EyeToy)
    bool hasLed;
    uint32_t controlMask;           // CTRL_* the Settings dialog should enable
    const ControlRange* ranges = nullptr;   // optional; null = all continuous
    uint32_t rangeCount = 0;
};

// The ControlRange for `cap`, or nullptr when the control is continuous.
inline const ControlRange* FindRange(const DeviceProfile* p, uint32_t cap)
{
    if (!p || !p->ranges)
        return nullptr;
    for (uint32_t i = 0; i < p->rangeCount; ++i)
        if (p->ranges[i].cap & cap)
            return &p->ranges[i];
    return nullptr;
}

// True if `p` advertises a capture mode exactly matching (w,h,fps). Shared by
// the host's initial mode pick and its hot-plug re-advertise so both agree on
// what a device can serve.
inline bool ProfileHasMode(const DeviceProfile& p, uint32_t w, uint32_t h, uint32_t fps)
{
    for (uint32_t i = 0; i < p.modeCount; ++i)
        if (p.modes[i].width == w && p.modes[i].height == h && p.modes[i].fps == fps)
            return true;
    return false;
}

// One acquired frame. Buffers are owned by the device and valid only until the
// next AcquireFrame/Stop on the same device. yuy2 is always present on success;
// jpeg is null for devices without a JFIF sidecar (PS3 Eye).
struct AcquiredFrame
{
    const uint8_t* yuy2;
    uint32_t       yuy2Bytes;
    const uint8_t* jpeg;
    uint32_t       jpegBytes;
};

class ICameraDevice
{
public:
    virtual ~ICameraDevice() = default;

    // Open USB and configure the sensor for `mode`. Returns false on failure,
    // leaving the device fully stopped (USB released).
    virtual bool Init(const VideoMode& mode) = 0;

    // Begin streaming. Returns false if transfers could not be submitted
    // (device left stopped).
    virtual bool Start() = 0;

    // Stop streaming; sensor off, LED off, 0% CPU. Idempotent.
    virtual void Stop() = 0;

    // Pull the next frame into device-owned buffers. Blocks up to timeoutMs;
    // returns false on timeout or when streaming stopped. wantJpeg lets a
    // JPEG-native device skip retaining the JFIF when no MJPEG client is active;
    // devices without a JPEG path ignore it and set out.jpeg = nullptr.
    virtual bool AcquireFrame(AcquiredFrame& out, uint32_t timeoutMs, bool wantJpeg) = 0;

    // Push sensor settings (gain/exposure/WB/flip/...). Safe live or pre-start.
    virtual void ApplySettings(const Settings& s) = 0;

    virtual const DeviceProfile& Profile() const = 0;

    // Change the logical view WITHOUT restarting the stream (PS4 only: Left/Right/
    // SideBySide are just different crops/compositions of the same live frame, so
    // switching needs no device interaction — avoiding the iso restart that would
    // otherwise degrade the OV580). Returns true if handled; false (the default)
    // means the caller must re-create the device to change the view. `viewKind` is
    // a Ps4ViewKind. A true return with a geometry change also updates Profile().
    virtual bool SetView(int /*viewKind*/) { return false; }

    // The mode the device is ACTUALLY producing after a successful Init().
    // Normally the requested mode, but a device that shares ONE hardware stream
    // between several logical slots (the PS4 split pair) can only honour one
    // geometry, so the non-owning half adopts whatever the shared engine runs.
    // The caller must advertise THIS, not what it asked for: a FrameBus sized
    // for frames that never arrive starves the stream to a few fps, which is
    // how this presents when it is got wrong.
    virtual VideoMode ActualMode(const VideoMode& requested) const { return requested; }

    // ---- optional microphone ---------------------------------------------
    // Only the PS4 camera implements these, and only because its 4-mic array is
    // embedded in the VIDEO stream rather than exposed as a USB-audio function
    // (the PS3 Eye and EyeToy mics are ordinary usbaudio endpoints that Windows
    // owns, and this project never touches them). Channel count is 0 when the
    // device has no in-band audio, which is the default.
    virtual uint32_t AudioChannels() const { return 0; }
    virtual uint32_t AudioSampleRate() const { return 0; }
    // The rate audio is REALLY arriving at, in milli-Hz; 0 when not yet known or
    // not applicable. AudioSampleRate() is the nominal rate the stream claims,
    // and on the PS4 the two differ: its camera runs at 60.029 fps rather than
    // 60.000, so the real rate is ~48,023 Hz and varies per video mode. A file
    // written at the nominal rate plays slow, and a live render into a
    // fixed-rate endpoint drifts until it over- or under-runs, so anything
    // lining this stream up against real time must use THIS.
    virtual uint32_t AudioMeasuredRateMilliHz() const { return 0; }
    // Copy up to `frames` interleaved sample frames; returns how many were taken.
    virtual uint32_t ReadAudio(int16_t* /*dst*/, uint32_t /*frames*/) { return 0; }
    // Per-channel RMS 0..1 of the most recent frame; false if unavailable.
    virtual bool AudioLevels(float* /*out*/, uint32_t /*count*/) { return false; }
    // Concealed audio gaps this session. Non-zero means video frames are being
    // dropped and the microphone is being spliced -- audible on speech.
    virtual uint32_t AudioDropouts() const { return 0; }
};
