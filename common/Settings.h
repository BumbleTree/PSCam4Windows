#pragma once
//
// Persistent configuration, stored under HKLM\SOFTWARE\PSCam4Win so it is
// writable by the (elevated) tray app and readable everywhere. The FrameBus
// header remains the DLL's source of truth for the active format; the
// registry is the *host's* source of truth across restarts.
//
#include <cstdint>
#include <string>

struct VideoMode
{
    uint32_t width;
    uint32_t height;
    uint32_t fps;
};

// Native OV7720/OV534 modes (valid-video rates only, matching the sensor
// tables in ps3eye.cpp).
inline constexpr VideoMode kVideoModes[] = {
    { 640, 480,  75 },
    { 640, 480,  60 },   // default
    { 640, 480,  50 },
    { 640, 480,  40 },
    { 640, 480,  30 },
    { 640, 480,  15 },
    { 320, 240, 187 },
    { 320, 240, 150 },
    { 320, 240, 125 },
    { 320, 240, 100 },
    { 320, 240,  75 },
    { 320, 240,  60 },
    { 320, 240,  30 },
};
inline constexpr int kVideoModeCount = static_cast<int>(sizeof(kVideoModes) / sizeof(kVideoModes[0]));
inline constexpr int kDefaultModeIndex = 1;  // 640x480 @ 60

struct Settings
{
    uint32_t width    = 640;
    uint32_t height   = 480;
    uint32_t fps      = 60;
    bool     flipH    = false;
    bool     flipV    = false;
    bool     autoGain = true;     // also drives auto-exposure (AGC/AEC)
    // 32, not 20: this is a PS3 Eye scale (0..63) and on the PS4 it maps to the
    // ISP's 0..8, where 20 lands on 2 -- BELOW the camera's own default of 4,
    // for no reason other than the shared scale. Across the PS4's full gain
    // range luma rises far faster than temporal noise does, so noise RELATIVE
    // to signal DROPS as gain goes up: undershooting costs brightness and
    // apparent cleanliness at once. 32 maps to the camera's own default of 4.
    uint32_t gain     = 32;      // 0..63, used when autoGain == false
    uint32_t exposure = 120;     // 0..255, used when autoGain == false
    bool     autoWhiteBalance = true;
    uint32_t idleTimeoutMs    = 3000;

    uint32_t redBalance = 128;    // 0..255  -> reg 0x02 AWB red gain (manual WB)
    uint32_t blueBalance = 128;   // 0..255  -> reg 0x01 AWB blue gain (manual WB)
    uint32_t greenBalance = 128;  // 0..255  -> reg 0x03 AWB green gain (manual WB)
    bool     testPattern = false;

    // EyeToy (OV7648) sensor controls. The PS3 Eye ignores these; the EyeToy
    // ignores gain/exposure/flip/balance/testPattern (its OV7648 only exposes
    // brightness/saturation/AWB via the proven gspca ov519 path).
    uint32_t brightness = 127;    // 0..255  -> OV7648 reg 0x06 (Y brightness)

    // PS4 (OV580 ISP) controls. Both are plain sliders: the ISP's auto-exposure
    // is permanently on and these ride on top of it, so neither is gated behind
    // an "auto off" toggle.
    uint32_t contrast = 127;      // 0..255 -> PU_CONTRAST 0..8 (tone curve)
    uint32_t wbTemp   = 128;      // 0..255 -> PU_WB_TEMP 2800..6500 K (AWB off)
    // 128 rather than 127 on purpose: on the PS4 these scale to the ISP's 0..8
    // and integer truncation makes 127 land on 3, one step BELOW the camera's
    // own default of 4 -- i.e. enabling the control would quietly desaturate and
    // soften the picture versus not writing it at all. brightness/contrast keep
    // 127 deliberately: on those, device value 4 and up clips highlights, which
    // is the blow-out this camera is reported for.
    uint32_t saturation = 128;    // 0..255  -> OV7648 reg 0x03 / PU_SATURATION 0..8
    uint32_t sharpness  = 128;    // 0..255  -> PU_SHARPNESS 0..8 (PS4 ISP)
    // Mains anti-flicker: 0 = off, 1 = 50 Hz, 2 = 60 Hz. The ISP powers up at
    // 50 Hz, so a 60 Hz region needs this written or the picture bands.
    uint32_t powerlineFreq = 1;
    // PS4 microphone array gain, 0..100 -> AK5703 registers 0x07/0x17 over their
    // useful 0x80..0xff span (~0.38 dB a step). Default 60 is deliberately not
    // the loudest usable value: clipping an analog gain is unrecoverable, while
    // too quiet can be made up downstream, so the default is sized for someone
    // sitting CLOSE to the camera and the slider covers the rest.
    uint32_t micGain  = 60;       // 0..100 %

    // PS4 camera (ignored by the PS3 Eye / EyeToy). ps4View selects which logical
    // view a *switchable* PS4 slot presents — a raw Ps4ViewKind: 0=Left, 1=Right,
    // 2=SideBySide (the unsuffixed "PS4 Camera" brand, the default). Stored as a
    // plain uint32_t so Settings.h needn't pull in ICameraDevice.h (which includes
    // this header). ps4Split, when set, makes one PS4 expose Left+Right as two
    // simultaneous cameras (claims two slots).
    uint32_t ps4View  = 2;        // Ps4ViewKind::SideBySide
    bool     ps4Split = false;
    // FNV-1a hash of the USB port path of the camera that ps4Split was set for.
    // ps4Split is honored only when this matches the camera currently in the slot,
    // so a DIFFERENT camera reusing this slot index can't inherit a stale split and
    // silently claim a second slot. 0 is the legacy wildcard (a split saved before
    // owner tagging, or with the camera briefly absent): honored for ANY camera so
    // pre-upgrade setups keep working; it is stamped with a real owner on the next
    // Split toggle. See deviceregistry::Ps4OwnerHash.
    uint32_t ps4SplitOwner = 0;

    bool SameMode(const Settings& o) const
    {
        return width == o.width && height == o.height && fps == o.fps;
    }
};

// ---------------------------------------------------------------------------
// Sensor/image controls: the subset of Settings that a device pushes to hardware
// through ICameraDevice::ApplySettings. Video mode (width/height/fps) and
// idleTimeoutMs are deliberately NOT here — a mode change goes through the
// queued re-advertise path instead, not a live control write.
//
// These two functions are a PAIR and must list exactly the same fields: one
// decides whether anything changed, the other copies the new values across.
// They live next to the struct on purpose, because a control added to Settings
// without editing both is dropped SILENTLY — the slider moves, the value
// persists, and the camera never sees it. Add a control here, once, and both
// paths pick it up. unit.settings.sensorcontrols fails if one is missed.
// ---------------------------------------------------------------------------
inline bool SensorControlsEqual(const Settings& a, const Settings& b)
{
    return a.flipH == b.flipH && a.flipV == b.flipV &&
           a.autoGain == b.autoGain && a.gain == b.gain &&
           a.exposure == b.exposure &&
           a.autoWhiteBalance == b.autoWhiteBalance &&
           a.redBalance == b.redBalance && a.blueBalance == b.blueBalance &&
           a.greenBalance == b.greenBalance && a.testPattern == b.testPattern &&
           a.brightness == b.brightness && a.saturation == b.saturation &&
           a.contrast == b.contrast && a.wbTemp == b.wbTemp &&
           a.micGain == b.micGain && a.sharpness == b.sharpness &&
           a.powerlineFreq == b.powerlineFreq;
}

inline void CopySensorControls(const Settings& from, Settings& to)
{
    to.flipH = from.flipH;       to.flipV = from.flipV;
    to.autoGain = from.autoGain; to.gain = from.gain;
    to.exposure = from.exposure; to.autoWhiteBalance = from.autoWhiteBalance;
    to.redBalance = from.redBalance; to.blueBalance = from.blueBalance;
    to.greenBalance = from.greenBalance; to.testPattern = from.testPattern;
    to.brightness = from.brightness; to.saturation = from.saturation;
    to.contrast = from.contrast; to.wbTemp = from.wbTemp;
    to.micGain = from.micGain; to.sharpness = from.sharpness;
    to.powerlineFreq = from.powerlineFreq;
}


namespace settings
{
    // ---- global audio-render target (NOT per-camera) ----------------------
    // The MMDevice id of the output the PS4 microphone is rendered into, or
    // empty for off. Deliberately outside `Settings`: there is one renderer,
    // not one per camera slot, and Settings is an all-DWORD struct whose three
    // parallel field lists (struct / Load / Save) are guarded by a sizeof
    // tripwire -- adding a std::wstring there would trip it for no benefit.
    //
    // The ID is persisted rather than the friendly name because names are not
    // unique and change with driver revisions, while the id is stable.
    std::wstring LoadMicRenderDevice();
    void         SaveMicRenderDevice(const std::wstring& deviceId);

    // Whether routing may keep the camera STREAMING with no video client, which
    // is the only way in-band audio can exist while nothing is watching. Global
    // for the same two reasons as the device id: there is one renderer, and the
    // controller's Asleep pass watches this key (not its Camera%d subkeys), so a
    // per-camera value could never wake a sleeping slot.
    //
    // Defaults to TRUE -- a microphone that dies whenever no app has the camera
    // open is not a microphone. It only has effect while an output is selected;
    // with routing off there is nothing to keep awake for.
    bool LoadMicKeepAwake();
    void SaveMicKeepAwake(bool on);

    // ---- global UI theme -------------------------------------------------
    // One window, so one value; it
    // lives beside the render target rather than in `Settings` for the same
    // reason -- that struct is per-camera and guarded by a sizeof tripwire.
    // 0 = follow Windows (the default), 1 = dark, 2 = light.
    uint32_t LoadUiTheme();
    void     SaveUiTheme(uint32_t mode);

    Settings Defaults();                                   // factory defaults for one camera
    Settings Load(int cameraIndex = 0);                    // missing values -> struct defaults
    bool     Save(int cameraIndex, const Settings& s);   // full write
    void     SeedDefaults(int cameraIndex = 0);            // write only values not yet present
    int      FindModeIndex(uint32_t w, uint32_t h, uint32_t fps);  // -1 if unknown
}
