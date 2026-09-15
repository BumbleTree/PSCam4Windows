#pragma once
//
// ControlModel — the one description of every camera control.
//
// A control is one table row. Which rows a device shows, what each is worth,
// how it is labelled and when it is greyed all come from here, so adding a
// control means adding a row and nothing else.
//
// Two fields carry exceptions a flat table would otherwise erase:
//
//   Interlock::whenCap  binds the interlock only when the device advertises the
//                       capability that owns it. The PS4 has CTRL_ISPGAIN and no
//                       auto-gain toggle, so an unconditional "auto gain must be
//                       off" would grey its gain slider for good.
//   sharedHw            false for controls that are per-view rather than shared
//                       ISP state. Flip is software mirroring and stays live on
//                       the Right half of a split pair; the ISP controls do not.
//
#include <cstdint>
#include <cstdio>

#include "../ICameraDevice.h"
#include "../../common/ControlScale.h"
#include "../../common/Settings.h"

namespace model {

enum class Unit : uint8_t { Percent, Kelvin, Decibel };
enum class Sec  : uint8_t { Image, WhiteBal, Orientation, Power, Mic, Count };

struct Interlock
{
    uint32_t         whenCap;        // 0 = never binds
    bool Settings::* mustBeFalse;
};

inline constexpr Interlock kNoLock    { 0,             nullptr };
inline constexpr Interlock kManualGain{ CTRL_GAIN,     &Settings::autoGain };
inline constexpr Interlock kManualWb  { CTRL_WHITEBAL, &Settings::autoWhiteBalance };

struct SliderDef
{
    uint32_t             cap;        // CTRL_* OR-set; any bit present shows the row
    Sec                  section;
    const wchar_t*       label;
    uint32_t Settings::* field;
    uint32_t             hi;         // stored range is [0, hi]
    Unit                 unit;
    Interlock            lock;
    bool                 sharedHw;
};

struct ToggleDef
{
    uint32_t         cap;
    Sec              section;
    const wchar_t*   label;
    bool Settings::* field;
    Interlock        lock;
    bool             sharedHw;
    bool             asSwitch;
};

using S = Settings;

inline constexpr SliderDef kSliders[] = {
//  capability              section          label           field         hi   unit           interlock    sharedHw
  { CTRL_GAIN|CTRL_ISPGAIN, Sec::Image,     L"Gain",        &S::gain,        63, Unit::Percent, kManualGain, true },
  { CTRL_EXPOSURE,          Sec::Image,     L"Exposure",    &S::exposure,   255, Unit::Percent, kManualGain, true },
  { CTRL_BRIGHTNESS,        Sec::Image,     L"Brightness",  &S::brightness, 255, Unit::Percent, kNoLock,     true },
  { CTRL_CONTRAST,          Sec::Image,     L"Contrast",    &S::contrast,   255, Unit::Percent, kNoLock,     true },
  { CTRL_SATURATION,        Sec::Image,     L"Saturation",  &S::saturation, 255, Unit::Percent, kNoLock,     true },
  { CTRL_SHARPNESS,         Sec::Image,     L"Sharpness",   &S::sharpness,  255, Unit::Percent, kNoLock,     true },
  { CTRL_WB_MANUAL,         Sec::WhiteBal,  L"Red",         &S::redBalance, 255, Unit::Percent, kManualWb,   true },
  { CTRL_WB_MANUAL,         Sec::WhiteBal,  L"Green",       &S::greenBalance,255,Unit::Percent, kManualWb,   true },
  { CTRL_WB_MANUAL,         Sec::WhiteBal,  L"Blue",        &S::blueBalance,255, Unit::Percent, kManualWb,   true },
  { CTRL_WB_TEMP,           Sec::WhiteBal,  L"Temperature", &S::wbTemp,     255, Unit::Kelvin,  kManualWb,   true },
  { CTRL_MICARRAY,          Sec::Mic,       L"Mic gain",    &S::micGain,    100, Unit::Decibel, kNoLock,     true },
};
inline constexpr int kSliderCount = (int)(sizeof(kSliders) / sizeof(kSliders[0]));

inline constexpr ToggleDef kToggles[] = {
//  capability        section            label                  field           interlock  sharedHw  switch
  { CTRL_GAIN,        Sec::Image,       L"Auto gain",          &S::autoGain,         kNoLock, true,  true  },
  { CTRL_TESTPATTERN, Sec::Image,       L"Test pattern",       &S::testPattern,      kNoLock, true,  false },
  { CTRL_WHITEBAL,    Sec::WhiteBal,    L"Auto white balance", &S::autoWhiteBalance, kNoLock, true,  true  },
  { CTRL_FLIP,        Sec::Orientation, L"Flip horizontally",  &S::flipH,            kNoLock, false, false },
  { CTRL_FLIP,        Sec::Orientation, L"Flip vertically",    &S::flipV,            kNoLock, false, false },
};
inline constexpr int kToggleCount = (int)(sizeof(kToggles) / sizeof(kToggles[0]));

inline const wchar_t* SectionName(Sec s)
{
    switch (s)
    {
    case Sec::Image:       return L"IMAGE";
    case Sec::WhiteBal:    return L"WHITE BALANCE";
    case Sec::Orientation: return L"ORIENTATION";
    case Sec::Power:       return L"POWER";
    case Sec::Mic:         return L"MICROPHONE";
    default:               return L"";
    }
}

// An interlock that does not apply to this device cannot grey anything.
inline bool Satisfied(const Interlock& k, uint32_t mask, const Settings& s)
{
    return !k.whenCap || !(mask & k.whenCap) || !(s.*k.mustBeFalse);
}

inline bool Shown(uint32_t cap, uint32_t mask) { return (mask & cap) != 0; }

inline bool Live(const SliderDef& d, uint32_t mask, const Settings& s, bool ownsShared)
{
    return Satisfied(d.lock, mask, s) && (ownsShared || !d.sharedHw);
}

inline bool Live(const ToggleDef& d, uint32_t mask, const Settings& s, bool ownsShared)
{
    return Satisfied(d.lock, mask, s) && (ownsShared || !d.sharedHw);
}

// Detent count for this control on this device; 0 = continuous.
inline uint32_t StepsFor(const SliderDef& d, const DeviceProfile* prof)
{
    const ControlRange* r = FindRange(prof, d.cap);
    return r ? r->steps : 0;
}

// The label beside the slider. Always the outcome, never the stored integer.
inline void FormatValue(const SliderDef& d, const DeviceProfile* prof,
                        uint32_t stored, wchar_t* out, size_t cap)
{
    const ControlRange* r = FindRange(prof, d.cap);

    if (r && r->steps > 1)
    {
        const uint32_t dev = ScaleToDevice(stored, d.hi, r->steps - 1);
        swprintf_s(out, cap, L"%u / %u", dev, r->steps - 1);
        return;
    }

    switch (d.unit)
    {
    case Unit::Kelvin:
    {
        const int lo = r ? r->uMin : 2800;
        const int hi = r ? r->uMax : 6500;
        swprintf_s(out, cap, L"%d K", lo + (int)ScaleToDevice(stored, d.hi, (uint32_t)(hi - lo)));
        break;
    }
    case Unit::Decibel:
    {
        // The array's analogue gain register spans 0x80..0xff at ~0.38 dB a
        // step, referenced to the 0xa8 the bring-up table leaves behind. A
        // percentage says nothing; "+14 dB" says how much louder than stock.
        const int reg = 0x80 + (int)stored * (0xff - 0x80) / 100;
        const double db = (reg - 0xa8) * 0.38;
        swprintf_s(out, cap, L"%+d dB", (int)(db + (db >= 0 ? 0.5 : -0.5)));
        break;
    }
    default:
        swprintf_s(out, cap, L"%u %%", d.hi ? (stored * 100 + d.hi / 2) / d.hi : 0);
        break;
    }
}

} // namespace model
