#include "Settings.h"
#include <windows.h>
#include <cstdio>

namespace
{

bool ReadDword(HKEY key, const wchar_t* name, uint32_t* out)
{
    DWORD value = 0, size = sizeof(value), type = 0;
    if (RegQueryValueExW(key, name, nullptr, &type,
                         reinterpret_cast<BYTE*>(&value), &size) != ERROR_SUCCESS ||
        type != REG_DWORD)
        return false;
    *out = value;
    return true;
}

bool ReadBool(HKEY key, const wchar_t* name, bool* out)
{
    uint32_t v;
    if (!ReadDword(key, name, &v))
        return false;
    *out = v != 0;
    return true;
}

void WriteDword(HKEY key, const wchar_t* name, uint32_t value)
{
    RegSetValueExW(key, name, 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

uint32_t Clamp(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

// Every value this file touches hangs off one key. Named once so the path
// cannot drift between a reader and its writer.
constexpr wchar_t kRootKey[] = L"SOFTWARE\\PSCam4Win";

// Camera 0 lives at the root key; 1..N-1 in Camera%d subkeys.
void FormatSettingsKeyPath(wchar_t (&buf)[128], int cameraIndex)
{
    if (cameraIndex == 0)
        swprintf_s(buf, L"%s", kRootKey);
    else
        swprintf_s(buf, L"%s\\Camera%d", kRootKey, cameraIndex);
}

// The global (not per-camera) values below share one key and one shape, so they
// share these two openers. Both return nullptr when the key is unavailable; the
// callers fall back to their documented defaults.
HKEY OpenRootRead()
{
    HKEY key = nullptr;
    return RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRootKey, 0, KEY_READ, &key) == ERROR_SUCCESS
               ? key : nullptr;
}

HKEY OpenRootWrite()
{
    HKEY key = nullptr;
    return RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRootKey, 0, nullptr, 0,
                           KEY_WRITE, nullptr, &key, nullptr) == ERROR_SUCCESS
               ? key : nullptr;
}

} // namespace

namespace settings
{

Settings Defaults()
{
    return Settings{};  // struct in-class defaults are the sensor factory values
}

int FindModeIndex(uint32_t w, uint32_t h, uint32_t fps)
{
    for (int i = 0; i < kVideoModeCount; ++i)
        if (kVideoModes[i].width == w && kVideoModes[i].height == h && kVideoModes[i].fps == fps)
            return i;
    return -1;
}

Settings Load(int cameraIndex)
{
    Settings s;  // struct defaults
    HKEY key = nullptr;
    wchar_t subKeyPath[128];
    FormatSettingsKeyPath(subKeyPath, cameraIndex);

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, subKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return s;

    ReadDword(key, L"Width",    &s.width);
    ReadDword(key, L"Height",   &s.height);
    ReadDword(key, L"Fps",      &s.fps);
    ReadBool (key, L"FlipH",    &s.flipH);
    ReadBool (key, L"FlipV",    &s.flipV);
    ReadBool (key, L"AutoGain", &s.autoGain);
    ReadDword(key, L"Gain",     &s.gain);
    ReadDword(key, L"Exposure", &s.exposure);
    ReadBool (key, L"AutoWhiteBalance", &s.autoWhiteBalance);
    ReadDword(key, L"IdleTimeoutMs",    &s.idleTimeoutMs);

    ReadDword(key, L"RedBalance",   &s.redBalance);
    ReadDword(key, L"BlueBalance",  &s.blueBalance);
    ReadDword(key, L"GreenBalance", &s.greenBalance);
    // Legacy Hue key ignored — old builds wrote AWB blue gain (reg 0x01) here.
    ReadBool (key, L"TestPattern",  &s.testPattern);

    ReadDword(key, L"Brightness",   &s.brightness);
    ReadDword(key, L"Contrast",     &s.contrast);
    ReadDword(key, L"WbTemp",       &s.wbTemp);
    ReadDword(key, L"MicGain",      &s.micGain);
    ReadDword(key, L"Sharpness",    &s.sharpness);
    ReadDword(key, L"PowerlineFreq",&s.powerlineFreq);
    ReadDword(key, L"Saturation",   &s.saturation);

    ReadDword(key, L"Ps4View",       &s.ps4View);
    ReadBool (key, L"Ps4Split",      &s.ps4Split);
    ReadDword(key, L"Ps4SplitOwner", &s.ps4SplitOwner);

    RegCloseKey(key);

    // Sanitize geometry. Reset only obviously-invalid values: a mode that is
    // simply not in the PS3 table (e.g. the EyeToy's 320x240@15) is left intact
    // and validated/clamped by the device-aware layer (CaptureController::Start
    // and the Settings dialog), so per-device modes persist correctly. The caps
    // mirror the FrameBus reader's per-axis sanity bound (4096), NOT any one
    // device's maximum: the PS4's Side-by-side view is 2560 wide, so the old
    // PS3-era 1920x1080 cap silently reset a persisted SBS mode on every load.
    if (s.width == 0 || s.height == 0 || s.fps == 0 ||
        s.width > 4096 || s.height > 4096 || s.fps > 240)
    {
        const VideoMode& def = kVideoModes[kDefaultModeIndex];
        s.width = def.width; s.height = def.height; s.fps = def.fps;
    }
    s.gain     = Clamp(s.gain, 0, 63);
    s.exposure = Clamp(s.exposure, 0, 255);
    s.idleTimeoutMs = Clamp(s.idleTimeoutMs, 1000, 60000);
    s.redBalance = Clamp(s.redBalance, 0, 255);
    s.blueBalance = Clamp(s.blueBalance, 0, 255);
    s.greenBalance = Clamp(s.greenBalance, 0, 255);
    s.brightness = Clamp(s.brightness, 0, 255);
    s.contrast   = Clamp(s.contrast, 0, 255);
    s.wbTemp     = Clamp(s.wbTemp, 0, 255);
    s.micGain    = Clamp(s.micGain, 0, 100);
    s.sharpness  = Clamp(s.sharpness, 0, 255);
    s.powerlineFreq = Clamp(s.powerlineFreq, 0, 2);
    s.saturation = Clamp(s.saturation, 0, 255);
    s.ps4View    = Clamp(s.ps4View, 0, 2);   // Ps4ViewKind range (Left..SideBySide)

    return s;
}

bool Save(int cameraIndex, const Settings& s)
{
    HKEY key = nullptr;
    wchar_t subKeyPath[128];
    FormatSettingsKeyPath(subKeyPath, cameraIndex);

    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, subKeyPath, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    WriteDword(key, L"Width",    s.width);
    WriteDword(key, L"Height",   s.height);
    WriteDword(key, L"Fps",      s.fps);
    WriteDword(key, L"FlipH",    s.flipH ? 1 : 0);
    WriteDword(key, L"FlipV",    s.flipV ? 1 : 0);
    WriteDword(key, L"AutoGain", s.autoGain ? 1 : 0);
    WriteDword(key, L"Gain",     s.gain);
    WriteDword(key, L"Exposure", s.exposure);
    WriteDword(key, L"AutoWhiteBalance", s.autoWhiteBalance ? 1 : 0);
    WriteDword(key, L"IdleTimeoutMs",    s.idleTimeoutMs);

    WriteDword(key, L"RedBalance",   s.redBalance);
    WriteDword(key, L"BlueBalance",  s.blueBalance);
    WriteDword(key, L"GreenBalance", s.greenBalance);
    WriteDword(key, L"TestPattern",  s.testPattern ? 1 : 0);

    WriteDword(key, L"Brightness",   s.brightness);
    WriteDword(key, L"Contrast",     s.contrast);
    WriteDword(key, L"WbTemp",       s.wbTemp);
    WriteDword(key, L"MicGain",      s.micGain);
    WriteDword(key, L"Sharpness",    s.sharpness);
    WriteDword(key, L"PowerlineFreq",s.powerlineFreq);
    WriteDword(key, L"Saturation",   s.saturation);

    WriteDword(key, L"Ps4View",       s.ps4View);
    WriteDword(key, L"Ps4Split",      s.ps4Split ? 1 : 0);
    WriteDword(key, L"Ps4SplitOwner", s.ps4SplitOwner);

    RegCloseKey(key);
    return true;
}

void SeedDefaults(int cameraIndex)
{
    // Load() falls back to defaults for anything missing; writing the merge
    // back persists defaults without clobbering existing user values.
    Save(cameraIndex, Load(cameraIndex));
}


// ---- global audio-render target -------------------------------------------
std::wstring LoadMicRenderDevice()
{
    HKEY key = OpenRootRead();
    if (!key)
        return {};
    wchar_t buf[512];
    DWORD cb = sizeof(buf), type = 0;
    std::wstring out;
    if (RegQueryValueExW(key, L"MicRenderDevice", nullptr, &type,
                         (LPBYTE)buf, &cb) == ERROR_SUCCESS && type == REG_SZ)
    {
        // Bound by the bytes the registry actually returned. REG_SZ is not
        // guaranteed to be terminated, so building the string from `buf` alone
        // would read past whatever was stored.
        size_t chars = cb / sizeof(wchar_t);
        if (chars > _countof(buf))
            chars = _countof(buf);
        while (chars && buf[chars - 1] == L'\0')
            --chars;                       // drop the terminator(s) if present
        out.assign(buf, chars);
    }
    RegCloseKey(key);
    return out;
}

void SaveMicRenderDevice(const std::wstring& deviceId)
{
    HKEY key = OpenRootWrite();
    if (!key)
        return;
    if (deviceId.empty())
        RegDeleteValueW(key, L"MicRenderDevice");
    else
        RegSetValueExW(key, L"MicRenderDevice", 0, REG_SZ,
                       (const BYTE*)deviceId.c_str(),
                       (DWORD)((deviceId.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

bool LoadMicKeepAwake()
{
    HKEY key = OpenRootRead();
    if (!key)
        return true;                       // absent -> on, see the header
    uint32_t v = 1;
    if (!ReadDword(key, L"MicKeepAwake", &v))
        v = 1;
    RegCloseKey(key);
    return v != 0;
}

void SaveMicKeepAwake(bool on)
{
    HKEY key = OpenRootWrite();
    if (!key)
        return;
    WriteDword(key, L"MicKeepAwake", on ? 1u : 0u);
    RegCloseKey(key);
}

// ---- global UI theme -------------------------------------------------------
// 0 = follow Windows, 1 = dark, 2 = light. Out-of-range collapses to 0 on BOTH
// sides: a writer that clamped to a different value than the reader would turn
// one bad write into a silent theme change rather than a rejected one.
uint32_t LoadUiTheme()
{
    HKEY key = OpenRootRead();
    if (!key)
        return 0;                          // absent -> follow Windows
    uint32_t v = 0;
    if (!ReadDword(key, L"UiTheme", &v) || v > 2)
        v = 0;
    RegCloseKey(key);
    return v;
}

void SaveUiTheme(uint32_t mode)
{
    HKEY key = OpenRootWrite();
    if (!key)
        return;
    WriteDword(key, L"UiTheme", mode > 2 ? 0u : mode);
    RegCloseKey(key);
}

} // namespace settings
