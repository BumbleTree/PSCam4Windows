#pragma once
//
// Theme — colour tokens, fonts and DPI metrics for the tray UI.
//
// Every widget draws from here; no colour literal exists outside Theme.cpp, so
// the light palette costs one table rather than a second implementation.
//
// UI thread only. Init() must run before the first widget is created. SetMode()
// and SetDpi() rebuild the cached GDI objects, so handles obtained from GetFont
// / Brush / Pen are valid only until one of them is called -- fetch them inside
// the paint that uses them.
//
#include <windows.h>
#include <cstdint>

namespace theme {

enum class Mode : uint32_t { System = 0, Dark = 1, Light = 2 };

struct Palette
{
    COLORREF bg;             // window ground
    COLORREF bgElevated;     // cards, chips, combo faces
    COLORREF bgInset;        // slider tracks, meter wells, preview mat
    COLORREF fg;
    COLORREF fgMuted;        // secondary text, section headers
    COLORREF fgDisabled;
    COLORREF accent;
    COLORREF accentHover;
    COLORREF accentPressed;
    COLORREF border;
    COLORREF borderFocus;
    COLORREF good;           // streaming, playing
    COLORREF warn;           // needs attention
    COLORREF bad;            // failed, clipping
};

void Init();
void Shutdown();

Mode GetMode();
void SetMode(Mode m);        // persists, rebuilds the caches
bool IsDark();

// The user's Windows "app colours" preference. Valid because the logon task runs
// as the interactive user, so HKCU is their hive.
bool SystemPrefersDark();

// Re-read the system preference; true when the effective palette changed.
bool OnSystemThemeChanged();

const Palette& C();

int  Dpi();
bool SetDpi(int dpi);        // true when it changed, i.e. the caller must relayout

// Bumped whenever the cached fonts, brushes and pens are rebuilt (theme or DPI
// change). Native controls hold a font handle and must re-fetch when it moves.
unsigned Generation();
int  Dp(int dip);            // device-independent px -> physical px

enum class Font { Body, Strong, Small, Caps };
HFONT  GetFont(Font f);
HBRUSH Brush(COLORREF c);
HPEN   Pen(COLORREF c, int width = 1);

// Dark title bar and border. Call before the first ShowWindow, or the window
// opens light and flips a frame later.
void ApplyWindowFrame(HWND hwnd);

// Fill `r` with `c`, and the rounded-rectangle variant used by every widget.
void Fill(HDC dc, const RECT& r, COLORREF c);
void FillRound(HDC dc, const RECT& r, int radiusDip, COLORREF c);
void FrameRound(HDC dc, const RECT& r, int radiusDip, COLORREF c, int widthDip = 1);

// DrawTextW with the theme font and colour already selected.
void Text(HDC dc, const RECT& r, const wchar_t* s, Font f, COLORREF c, UINT format);
int  TextWidth(HDC dc, const wchar_t* s, Font f);

} // namespace theme
