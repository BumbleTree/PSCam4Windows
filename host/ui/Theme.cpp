#include "Theme.h"

#include <dwmapi.h>
#include "../../common/Settings.h"

namespace theme {
namespace {

// The two palettes. This table is the only place a colour is written.
constexpr Palette kDark = {
    RGB(0x1B, 0x1B, 0x1F),   // bg
    RGB(0x23, 0x23, 0x28),   // bgElevated
    RGB(0x13, 0x13, 0x16),   // bgInset
    RGB(0xF2, 0xF2, 0xF5),   // fg
    RGB(0x9A, 0x9A, 0xA5),   // fgMuted
    RGB(0x5A, 0x5A, 0x64),   // fgDisabled
    RGB(0x4C, 0x9E, 0xFF),   // accent
    RGB(0x6F, 0xB2, 0xFF),   // accentHover
    RGB(0x3A, 0x82, 0xD8),   // accentPressed
    RGB(0x34, 0x34, 0x3C),   // border
    RGB(0x7A, 0xBD, 0xFF),   // borderFocus
    RGB(0x3F, 0xB9, 0x50),   // good
    RGB(0xD2, 0x99, 0x22),   // warn
    RGB(0xF8, 0x51, 0x49),   // bad
};

constexpr Palette kLight = {
    RGB(0xF6, 0xF6, 0xF8),
    RGB(0xFF, 0xFF, 0xFF),
    RGB(0xE6, 0xE6, 0xEB),
    RGB(0x1A, 0x1A, 0x1E),
    RGB(0x5E, 0x5E, 0x6B),
    RGB(0xAA, 0xAA, 0xB4),
    RGB(0x10, 0x6E, 0xBE),
    RGB(0x1A, 0x84, 0xDC),
    RGB(0x0B, 0x55, 0x94),
    RGB(0xD0, 0xD0, 0xD8),
    RGB(0x10, 0x6E, 0xBE),
    RGB(0x1A, 0x7F, 0x37),
    RGB(0x9A, 0x6D, 0x00),
    RGB(0xC4, 0x2B, 0x1C),
};

struct FontSpec { int tenthPt; int weight; };
constexpr FontSpec kFonts[] = {
    { 90,  FW_NORMAL },   // Body
    { 90,  FW_SEMIBOLD }, // Strong
    { 80,  FW_NORMAL },   // Small
    { 75,  FW_SEMIBOLD }, // Caps
};
constexpr int kFontCount = (int)(sizeof(kFonts) / sizeof(kFonts[0]));

// Small fixed caches. The palette has fourteen colours and pens come in one or
// two widths, so linear search over a handful of entries beats a map.
struct BrushEntry { COLORREF c; HBRUSH h; };
struct PenEntry   { COLORREF c; int w; HPEN h; };

Mode       g_mode      = Mode::Dark;
bool       g_dark      = true;
int        g_dpi       = 96;
HFONT      g_fonts[kFontCount] = {};
BrushEntry g_brushes[24] = {};
int        g_brushCount = 0;
PenEntry   g_pens[24] = {};
int        g_penCount  = 0;

void FreeGdi()
{
    for (int i = 0; i < kFontCount; ++i)
    {
        if (g_fonts[i]) DeleteObject(g_fonts[i]);
        g_fonts[i] = nullptr;
    }
    for (int i = 0; i < g_brushCount; ++i) DeleteObject(g_brushes[i].h);
    for (int i = 0; i < g_penCount; ++i)   DeleteObject(g_pens[i].h);
    g_brushCount = g_penCount = 0;
}

void BuildFonts()
{
    for (int i = 0; i < kFontCount; ++i)
    {
        LOGFONTW lf{};
        lf.lfHeight  = -MulDiv(kFonts[i].tenthPt, g_dpi, 720);
        lf.lfWeight  = kFonts[i].weight;
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        wcscpy_s(lf.lfFaceName, L"Segoe UI");
        g_fonts[i] = CreateFontIndirectW(&lf);
    }
}

unsigned   g_generation = 1;

void Rebuild()
{
    FreeGdi();
    BuildFonts();
    ++g_generation;
}

bool EffectiveDark(Mode m)
{
    switch (m)
    {
    case Mode::Dark:  return true;
    case Mode::Light: return false;
    default:          return SystemPrefersDark();
    }
}

} // namespace

void Init()
{
    g_mode = static_cast<Mode>(settings::LoadUiTheme());
    g_dark = EffectiveDark(g_mode);
    Rebuild();
}

void Shutdown() { FreeGdi(); }

Mode GetMode() { return g_mode; }
bool IsDark()  { return g_dark; }

void SetMode(Mode m)
{
    g_mode = m;
    g_dark = EffectiveDark(m);
    settings::SaveUiTheme(static_cast<uint32_t>(m));
    Rebuild();
}

bool SystemPrefersDark()
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return true;
    DWORD v = 0, cb = sizeof(v), type = 0;
    const bool ok = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, &type,
                                     (LPBYTE)&v, &cb) == ERROR_SUCCESS &&
                    type == REG_DWORD;
    RegCloseKey(key);
    return ok ? v == 0 : true;
}

bool OnSystemThemeChanged()
{
    const bool want = EffectiveDark(g_mode);
    if (want == g_dark)
        return false;
    g_dark = want;
    Rebuild();
    return true;
}

const Palette& C() { return g_dark ? kDark : kLight; }

unsigned Generation() { return g_generation; }

int Dpi() { return g_dpi; }
int Dp(int dip) { return MulDiv(dip, g_dpi, 96); }

bool SetDpi(int dpi)
{
    if (dpi <= 0 || dpi == g_dpi)
        return false;
    g_dpi = dpi;
    Rebuild();
    return true;
}

HFONT GetFont(Font f)
{
    const int i = static_cast<int>(f);
    return (i >= 0 && i < kFontCount) ? g_fonts[i] : g_fonts[0];
}

HBRUSH Brush(COLORREF c)
{
    for (int i = 0; i < g_brushCount; ++i)
        if (g_brushes[i].c == c) return g_brushes[i].h;
    if (g_brushCount == (int)(sizeof(g_brushes) / sizeof(g_brushes[0])))
        return g_brushes[0].h;
    HBRUSH h = CreateSolidBrush(c);
    g_brushes[g_brushCount++] = { c, h };
    return h;
}

HPEN Pen(COLORREF c, int width)
{
    for (int i = 0; i < g_penCount; ++i)
        if (g_pens[i].c == c && g_pens[i].w == width) return g_pens[i].h;
    if (g_penCount == (int)(sizeof(g_pens) / sizeof(g_pens[0])))
        return g_pens[0].h;
    HPEN h = CreatePen(PS_SOLID, width, c);
    g_pens[g_penCount++] = { c, width, h };
    return h;
}

void ApplyWindowFrame(HWND hwnd)
{
    const BOOL dark = g_dark ? TRUE : FALSE;
    // 20 is the documented attribute; 19 was its pre-20H1 number. Trying both
    // costs one failed call on modern builds and buys the older ones.
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark))))
        DwmSetWindowAttribute(hwnd, 19, &dark, sizeof(dark));
}

void Fill(HDC dc, const RECT& r, COLORREF c)
{
    FillRect(dc, &r, Brush(c));
}

void FillRound(HDC dc, const RECT& r, int radiusDip, COLORREF c)
{
    const int d = Dp(radiusDip) * 2;
    HGDIOBJ ob = SelectObject(dc, Brush(c));
    HGDIOBJ op = SelectObject(dc, Pen(c));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, d, d);
    SelectObject(dc, op);
    SelectObject(dc, ob);
}

void FrameRound(HDC dc, const RECT& r, int radiusDip, COLORREF c, int widthDip)
{
    const int d = Dp(radiusDip) * 2;
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    HGDIOBJ op = SelectObject(dc, Pen(c, Dp(widthDip)));
    RoundRect(dc, r.left, r.top, r.right, r.bottom, d, d);
    SelectObject(dc, op);
    SelectObject(dc, ob);
}

void Text(HDC dc, const RECT& r, const wchar_t* s, Font f, COLORREF c, UINT format)
{
    if (!s || !*s)
        return;
    HGDIOBJ of = SelectObject(dc, GetFont(f));
    const COLORREF oc = SetTextColor(dc, c);
    const int obk = SetBkMode(dc, TRANSPARENT);
    RECT rr = r;
    DrawTextW(dc, s, -1, &rr, format);
    SetBkMode(dc, obk);
    SetTextColor(dc, oc);
    SelectObject(dc, of);
}

int TextWidth(HDC dc, const wchar_t* s, Font f)
{
    if (!s || !*s)
        return 0;
    HGDIOBJ of = SelectObject(dc, GetFont(f));
    SIZE sz{};
    GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz);
    SelectObject(dc, of);
    return sz.cx;
}

} // namespace theme
