#pragma once
//
// Widgets — the owner-drawn control set for the tray UI.
//
// A widget is one child window per ROW, not per control: a slider row draws its
// label, track and value together. That keeps the window count low, makes a
// half-laid-out row impossible, and lets the meter bank repaint as a single
// rect at 10 Hz.
//
// The comctl32 trackbar and progress bar have no dark theme class, which is why
// these are drawn rather than themed. Combo boxes stay native.
//
// UI thread only. Widgets are children of the settings window and are destroyed
// with it.
//
#include <windows.h>
#include <functional>
#include <string>
#include <vector>

#include "Theme.h"

namespace ui {

// Registers the shared widget window class. Idempotent.
bool Register(HINSTANCE inst);

// Counts widget moves and visibility changes that ACTUALLY happened. Snapshot
// it around a layout pass: unchanged means nothing shifted, so the parent needs
// no repaint.
unsigned LayoutEpoch();

// Bracket a layout pass. Between these, Show() only records what it wants and
// EndLayoutPass applies the difference -- so the hide-everything-then-show-what-
// is-needed pattern costs nothing for widgets that stay visible either way.
void BeginLayoutPass();
void EndLayoutPass();

// The union of the parent area widgets moved off or hid during the last pass,
// cleared as it is taken. False when nothing was vacated, which means the
// dialog background needs no repainting at all.
bool TakeVacatedRegion(RECT& out);

class Widget
{
public:
    virtual ~Widget();

    HWND Hwnd() const { return _h; }
    void Move(const RECT& px);
    void Show(bool on);
    void ApplyPendingShow();   // EndLayoutPass only
    void Enable(bool on);
    bool IsEnabled() const { return _enabled; }
    void Invalidate();

protected:
    // `title` becomes the window text. An owner-drawn control exposes nothing
    // to assistive tech otherwise, and it is what lets a layout check assert
    // that a visible control says something.
    bool Create(HWND parent, const RECT& px, bool focusable, const wchar_t* title = nullptr);

    virtual void OnPaint(HDC dc, const RECT& client) = 0;
    // Return true when handled.
    virtual bool OnMouse(UINT msg, POINT pt) { (void)msg; (void)pt; return false; }
    virtual bool OnKey(WPARAM vk)            { (void)vk; return false; }
    virtual bool OnSetCursor()             { return false; }
    virtual void OnFocus(bool)               {}

    HWND _h        = nullptr;
    bool _enabled  = true;
    bool _hot      = false;
    bool _focused  = false;

private:
    friend bool Register(HINSTANCE);

    RECT _rect{};             // last applied position, so Move can no-op
    bool _shown       = false;  // last applied visibility, so Show can no-op
    bool _wantShown   = false;  // visibility requested during a layout pass
    bool _pendingMark = false;  // already queued for this pass

    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    void PaintBuffered();
    void ReleaseBuffer();

    HDC     _memDc  = nullptr;
    HBITMAP _memBmp = nullptr;
    HGDIOBJ _memOld = nullptr;
    int     _memW = 0, _memH = 0;
};

// ---------------------------------------------------------------------------

// A labelled slider. `steps` > 1 gives detents; 0 is continuous.
//
// onMove fires live while dragging (apply to hardware); onCommit fires once at
// the end of the interaction, for controls whose write is expensive.
class SliderRow : public Widget
{
public:
    bool Create(HWND parent, const RECT& px, const wchar_t* label);

    void SetRange(uint32_t hi, uint32_t steps);
    void SetValue(uint32_t stored);          // silent: never raises onMove
    uint32_t Value() const { return _stored; }
    void SetValueText(std::wstring t);

    std::function<void()> onMove;
    std::function<void()> onCommit;

protected:
    void OnPaint(HDC dc, const RECT& client) override;
    bool OnMouse(UINT msg, POINT pt) override;
    bool OnKey(WPARAM vk) override;
    void OnFocus(bool f) override { _focused = f; Invalidate(); }

private:
    RECT TrackRect(const RECT& client) const;
    void SetFromX(int x, const RECT& client);
    void Step(int delta);

    std::wstring _label, _valueText;
    uint32_t _stored = 0, _hi = 255, _steps = 0;
    bool     _dragging = false;
};

// Checkbox or switch, with its label.
class ToggleRow : public Widget
{
public:
    enum class Look { Check, Switch };

    bool Create(HWND parent, const RECT& px, const wchar_t* label, Look look);

    void SetChecked(bool on);                // silent
    bool Checked() const { return _on; }

    std::function<void()> onToggle;

protected:
    void OnPaint(HDC dc, const RECT& client) override;
    bool OnMouse(UINT msg, POINT pt) override;
    bool OnKey(WPARAM vk) override;
    void OnFocus(bool f) override { _focused = f; Invalidate(); }

private:
    void Flip();

    std::wstring _label;
    Look _look = Look::Check;
    bool _on      = false;
    bool _pressed = false;
};

// The microphone level meters: all channels in one window, so a level refresh
// is a single invalidate.
class MeterBank : public Widget
{
public:
    static constexpr int kMaxChannels = 4;

    bool Create(HWND parent, const RECT& px);

    // `rms` is 0..1 per channel. Returns true when the drawn state changed,
    // which is the caller's cue to invalidate -- a meter that has not visibly
    // moved must not repaint.
    bool SetLevels(const float* rms, int count, bool live);

protected:
    void OnPaint(HDC dc, const RECT& client) override;

private:
    struct Ch { int bar = 0; int peak = 0; DWORD clipUntil = 0; };
    Ch   _ch[kMaxChannels];
    int  _count = 0;
    bool _live  = false;
};

class Button : public Widget
{
public:
    enum class Kind { Primary, Secondary };

    bool Create(HWND parent, const RECT& px, const wchar_t* text, Kind kind);
    void SetText(const wchar_t* text);

    std::function<void()> onClick;

protected:
    void OnPaint(HDC dc, const RECT& client) override;
    bool OnMouse(UINT msg, POINT pt) override;
    bool OnKey(WPARAM vk) override;
    void OnFocus(bool f) override { _focused = f; Invalidate(); }

private:
    std::wstring _text;
    Kind _kind = Kind::Secondary;
    bool _pressed = false;
};

// A section rule: small-caps label with a hairline. Not focusable.
class SectionHeader : public Widget
{
public:
    bool Create(HWND parent, const RECT& px, const wchar_t* text);
protected:
    void OnPaint(HDC dc, const RECT& client) override;
private:
    std::wstring _text;
};

// Body text: one or more lines, optionally a link. Used for the microphone and
// routing explanations, and for the empty/error cards.
class InfoText : public Widget
{
public:
    bool Create(HWND parent, const RECT& px);

    // True when the text changed, i.e. the caller should invalidate.
    bool SetText(const wchar_t* text);
    void SetLink(bool on);
    void SetCentered(bool on)   { _centered = on; }
    void SetFont(theme::Font f) { _font = f; }
    void SetSingleLine(bool on) { _single = on; }   // vertically centred, no wrap

    std::function<void()> onClick;

protected:
    void OnPaint(HDC dc, const RECT& client) override;
    bool OnMouse(UINT msg, POINT pt) override;
    bool OnSetCursor() override;

private:
    std::wstring _text;
    theme::Font _font = theme::Font::Small;
    bool _link = false, _centered = false, _single = false;
};

// The header state indicator: a coloured dot plus a sentence.
class StatusPill : public Widget
{
public:
    bool Create(HWND parent, const RECT& px);
    bool Set(COLORREF dot, const wchar_t* text);   // true when it changed

protected:
    void OnPaint(HDC dc, const RECT& client) override;

private:
    std::wstring _text;
    COLORREF _dot = 0;
};

// One camera in the device selector.
class Chip : public Widget
{
public:
    bool Create(HWND parent, const RECT& px, const wchar_t* text);
    void SetText(const wchar_t* text);
    void SetSelected(bool on);
    void SetDot(COLORREF c);
    // Width this chip wants, in physical px, for the given DC.
    int  MeasureWidth(HDC dc) const;
    const std::wstring& Text() const { return _text; }

    std::function<void()> onClick;

protected:
    void OnPaint(HDC dc, const RECT& client) override;
    bool OnMouse(UINT msg, POINT pt) override;
    bool OnKey(WPARAM vk) override;
    void OnFocus(bool f) override { _focused = f; Invalidate(); }

private:
    std::wstring _text;
    COLORREF _dot = 0;
    bool _selected = false;
};

// ---------------------------------------------------------------------------
// Native combo box, themed as far as the platform allows and owner-drawn for
// its items. Kept native so the drop-down keeps its keyboard and mouse
// behaviour.
class Combo
{
public:
    bool Create(HWND parent, const RECT& px, int id);
    void Move(const RECT& px);
    void Show(bool on);
    void Enable(bool on);
    HWND Hwnd() const { return _h; }

    // Re-apply the font and theme class. A theme or DPI change rebuilds the
    // cached GDI objects, and the font handed to WM_SETFONT dies with them --
    // unlike the owner-drawn widgets, which fetch theirs inside each paint.
    void Restyle();

    void Clear();
    int  Add(const wchar_t* text, LPARAM data = 0);
    void SetCurSel(int i);
    int  CurSel() const;
    LPARAM Data(int i) const;
    void SetDroppedWidthPx(int px);

    // Called from the parent's WM_DRAWITEM / WM_MEASUREITEM.
    static void DrawItem(const DRAWITEMSTRUCT* d);
    static int  ItemHeightPx();

private:
    HWND _h = nullptr;
};

} // namespace ui
