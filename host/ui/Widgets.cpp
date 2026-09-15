#include "Widgets.h"

#include <vector>

#include <math.h>
#include <uxtheme.h>
#include <windowsx.h>

#include "Layout.h"
#include "../../common/ControlScale.h"

namespace ui {
namespace {

constexpr wchar_t kClass[] = L"PSCam4WinWidget";
bool g_registered = false;

// Row metrics, DIP.
constexpr int kLabelW  = 84;
constexpr int kValueW  = 58;
constexpr int kGap     = 10;
constexpr int kTrackH  = 4;
constexpr int kThumbR  = 7;
constexpr int kBoxSize = 15;   // checkbox
constexpr int kSwitchW = 30;
constexpr int kSwitchH = 16;
constexpr int kDotR    = 4;

int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// Detents are the device's own values, via the shared scale: the thumb, the
// label and the write all agree because all three come from one function.
uint32_t Canonical(uint32_t n, uint32_t hi, uint32_t steps)
{ return StoredForDevice(n, hi, steps - 1); }

uint32_t DetentOf(uint32_t stored, uint32_t hi, uint32_t steps)
{ return ScaleToDevice(stored, hi, steps - 1); }

void FillCircle(HDC dc, int cx, int cy, int r, COLORREF c)
{
    HGDIOBJ ob = SelectObject(dc, theme::Brush(c));
    HGDIOBJ op = SelectObject(dc, theme::Pen(c));
    Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
    SelectObject(dc, op);
    SelectObject(dc, ob);
}

} // namespace

// ---------------------------------------------------------------------------

bool Register(HINSTANCE inst)
{
    if (g_registered)
        return true;
    WNDCLASSW wc{};
    wc.style         = CS_DBLCLKS;
    wc.lpfnWndProc   = Widget::Proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClass;
    g_registered = RegisterClassW(&wc) != 0;
    return g_registered;
}

Widget::~Widget()
{
    ReleaseBuffer();
    // Windows destroys a parent's children with it, so at process teardown the
    // handle is usually already gone and only the stale value remains.
    if (_h && IsWindow(_h))
    {
        SetWindowLongPtrW(_h, GWLP_USERDATA, 0);
        DestroyWindow(_h);
    }
    _h = nullptr;
}

namespace {
// Bumped whenever a widget actually moves or changes visibility. A relayout
// that leaves every widget where it was does not need the parent repainted,
// and comparing this before and after is how the caller can tell.
unsigned g_layoutEpoch = 0;

// Visibility is DEFERRED inside a layout pass. A pass hides everything and then
// re-shows what the current camera needs, so applying immediately hides and
// re-shows the whole dialog every time -- two ShowWindow calls and two repaints
// per widget, for a window that usually ends up looking identical. Deferring
// collapses hide-then-show into nothing.
bool                 g_inPass = false;
std::vector<Widget*> g_pending;

// The parent area a layout pass left behind: the union of the rects widgets
// moved OFF or hid. A widget that moves or appears repaints itself, so only
// the ground it vacated is the dialog's problem -- and repainting just that
// beats erasing the whole window for a one-chip change.
RECT g_dirty  = { 0, 0, 0, 0 };
bool g_anyDirty = false;

void MarkVacated(const RECT& r)
{
    if (!g_anyDirty) { g_dirty = r; g_anyDirty = true; return; }
    if (r.left   < g_dirty.left)   g_dirty.left   = r.left;
    if (r.top    < g_dirty.top)    g_dirty.top    = r.top;
    if (r.right  > g_dirty.right)  g_dirty.right  = r.right;
    if (r.bottom > g_dirty.bottom) g_dirty.bottom = r.bottom;
}
}

bool TakeVacatedRegion(RECT& out)
{
    if (!g_anyDirty)
        return false;
    out = g_dirty;
    g_anyDirty = false;
    return true;
}

unsigned LayoutEpoch() { return g_layoutEpoch; }

void BeginLayoutPass()
{
    g_inPass = true;
    g_pending.clear();
    g_anyDirty = false;
}

void EndLayoutPass()
{
    g_inPass = false;
    for (Widget* w : g_pending)
        w->ApplyPendingShow();
    g_pending.clear();
}

bool Widget::Create(HWND parent, const RECT& px, bool focusable, const wchar_t* title)
{
    DWORD style = WS_CHILD | WS_VISIBLE;
    if (focusable)
        style |= WS_TABSTOP;
    _h = CreateWindowExW(0, kClass, title ? title : L"", style,
                         px.left, px.top, px.right - px.left, px.bottom - px.top,
                         parent, nullptr, (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE),
                         this);
    _rect      = px;
    _shown     = true;   // WS_VISIBLE above
    _wantShown = true;
    return _h != nullptr;
}

// Move and Show BOTH no-op when nothing changes, and that is load-bearing
// rather than a micro-optimisation. Relayout starts by hiding every widget and
// then re-shows them, so unguarded these two hide and re-show the whole dialog
// on every pass -- a full-window flicker even when not a single control moved.
void Widget::Move(const RECT& px)
{
    if (!_h)
        return;
    if (px.left == _rect.left && px.top == _rect.top &&
        px.right == _rect.right && px.bottom == _rect.bottom)
        return;
    MarkVacated(_rect);       // the ground this widget is leaving
    _rect = px;
    ++g_layoutEpoch;
    SetWindowPos(_h, nullptr, px.left, px.top,
                 px.right - px.left, px.bottom - px.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Widget::Show(bool on)
{
    if (!_h)
        return;
    if (g_inPass)
    {
        if (!_pendingMark)
        {
            _pendingMark = true;
            g_pending.push_back(this);
        }
        _wantShown = on;
        return;
    }
    if (_shown == on)
        return;
    _shown = on;
    ++g_layoutEpoch;
    if (!on) MarkVacated(_rect);
    ShowWindow(_h, on ? SW_SHOW : SW_HIDE);
}

void Widget::ApplyPendingShow()
{
    _pendingMark = false;
    if (!_h || _shown == _wantShown)
        return;
    _shown = _wantShown;
    ++g_layoutEpoch;
    if (!_shown) MarkVacated(_rect);
    ShowWindow(_h, _shown ? SW_SHOW : SW_HIDE);
}

void Widget::Enable(bool on)
{
    if (_enabled == on)
        return;
    _enabled = on;
    if (_h)
    {
        EnableWindow(_h, on);
        Invalidate();
    }
}

void Widget::Invalidate()
{
    if (_h) InvalidateRect(_h, nullptr, FALSE);
}

void Widget::ReleaseBuffer()
{
    if (_memDc)
    {
        SelectObject(_memDc, _memOld);
        DeleteObject(_memBmp);
        DeleteDC(_memDc);
        _memDc = nullptr;
        _memBmp = nullptr;
        _memW = _memH = 0;
    }
}

void Widget::PaintBuffered()
{
    PAINTSTRUCT ps{};
    HDC dc = BeginPaint(_h, &ps);
    RECT client{};
    GetClientRect(_h, &client);
    const int w = client.right, h = client.bottom;
    if (w > 0 && h > 0)
    {
        if (!_memDc || _memW != w || _memH != h)
        {
            ReleaseBuffer();
            _memDc  = CreateCompatibleDC(dc);
            _memBmp = CreateCompatibleBitmap(dc, w, h);
            if (_memDc && _memBmp)
            {
                _memOld = SelectObject(_memDc, _memBmp);
                _memW = w; _memH = h;
            }
        }
        if (_memDc && _memW == w)
        {
            theme::Fill(_memDc, client, theme::C().bg);
            OnPaint(_memDc, client);
            BitBlt(dc, 0, 0, w, h, _memDc, 0, 0, SRCCOPY);
        }
        else
        {
            theme::Fill(dc, client, theme::C().bg);
            OnPaint(dc, client);
        }
    }
    EndPaint(_h, &ps);
}

LRESULT CALLBACK Widget::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    Widget* self;
    if (msg == WM_NCCREATE)
    {
        self = static_cast<Widget*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_h = h;
    }
    else
    {
        self = reinterpret_cast<Widget*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    }
    if (!self)
        return DefWindowProcW(h, msg, wp, lp);

    switch (msg)
    {
    case WM_ERASEBKGND:
        return TRUE;

    case WM_PAINT:
        self->PaintBuffered();
        return 0;

    case WM_SETCURSOR:
        if (self->OnSetCursor())
            return TRUE;
        break;

    case WM_GETDLGCODE:
        // Arrows and characters, never Tab: the dialog manager keeps traversal.
        return DLGC_WANTARROWS | DLGC_WANTCHARS;

    case WM_SETFOCUS:
        self->_focused = true;
        self->OnFocus(true);
        return 0;

    case WM_KILLFOCUS:
        self->_focused = false;
        self->OnFocus(false);
        return 0;

    case WM_MOUSEMOVE:
        if (!self->_hot)
        {
            self->_hot = true;
            TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, h, 0 };
            TrackMouseEvent(&tme);
            self->Invalidate();
        }
        break;

    case WM_MOUSELEAVE:
        self->_hot = false;
        self->Invalidate();
        return 0;

    case WM_KEYDOWN:
        if (self->_enabled && self->OnKey(wp))
            return 0;
        break;

    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
        break;

    default:
        return DefWindowProcW(h, msg, wp, lp);
    }

    if (msg == WM_MOUSEMOVE || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
        msg == WM_LBUTTONDBLCLK || msg == WM_MOUSEWHEEL)
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        if (msg == WM_MOUSEWHEEL)
        {
            // Wheel coordinates are screen-relative.
            ScreenToClient(h, &pt);
            pt.y = GET_WHEEL_DELTA_WPARAM(wp);
        }
        if (self->_enabled && self->OnMouse(msg, pt))
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// SliderRow

bool SliderRow::Create(HWND parent, const RECT& px, const wchar_t* label)
{
    _label = label ? label : L"";
    return Widget::Create(parent, px, true, _label.c_str());
}

void SliderRow::SetRange(uint32_t hi, uint32_t steps)
{
    _hi    = hi ? hi : 255;
    _steps = steps;
}

void SliderRow::SetValue(uint32_t stored)
{
    if (stored > _hi) stored = _hi;
    if (_stored == stored)
        return;
    _stored = stored;
    Invalidate();
}

void SliderRow::SetValueText(std::wstring t)
{
    if (_valueText == t)
        return;
    _valueText = std::move(t);
    Invalidate();
}

RECT SliderRow::TrackRect(const RECT& client) const
{
    const layout::Cells c = layout::Split(client, kLabelW, kValueW, kGap);
    const int r = theme::Dp(kThumbR);
    RECT t = layout::VCenter(c.body, kTrackH);
    t.left  += r;
    t.right -= r;
    if (t.right < t.left) t.right = t.left;
    return t;
}

void SliderRow::OnPaint(HDC dc, const RECT& client)
{
    const theme::Palette& p = theme::C();
    const layout::Cells c = layout::Split(client, kLabelW, kValueW, kGap);
    const COLORREF fg   = _enabled ? p.fg : p.fgDisabled;
    const COLORREF tint = _enabled ? p.accent : p.fgDisabled;

    theme::Text(dc, c.label, _label.c_str(), theme::Font::Body, fg,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    theme::Text(dc, c.value, _valueText.c_str(), theme::Font::Body,
                _enabled ? p.fgMuted : p.fgDisabled,
                DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    const RECT track = TrackRect(client);
    const int tw = track.right - track.left;
    if (tw <= 0)
        return;

    // Draw the thumb on the detent when the device is stepped, so the picture
    // agrees with the value text.
    double pos = 0.0;
    if (_steps > 1)
        pos = (double)DetentOf(_stored, _hi, _steps) / (double)(_steps - 1);
    else if (_hi)
        pos = (double)_stored / (double)_hi;
    const int x = track.left + (int)(pos * tw + 0.5);

    theme::FillRound(dc, track, kTrackH / 2, p.bgInset);
    if (x > track.left)
    {
        RECT filled{ track.left, track.top, x, track.bottom };
        theme::FillRound(dc, filled, kTrackH / 2, tint);
    }

    if (_steps > 1 && _steps <= 16)
    {
        const int cy = (track.top + track.bottom) / 2;
        for (uint32_t i = 0; i < _steps; ++i)
        {
            const int tx = track.left + (int)((double)i / (_steps - 1) * tw + 0.5);
            FillCircle(dc, tx, cy, theme::Dp(1),
                       tx <= x ? p.bgInset : (_enabled ? p.border : p.bgInset));
        }
    }

    const int cy = (track.top + track.bottom) / 2;
    const int r  = theme::Dp(kThumbR);
    if (_focused && _enabled)
        FillCircle(dc, x, cy, r + theme::Dp(2), p.borderFocus);
    FillCircle(dc, x, cy, r, _enabled ? (_hot || _dragging ? p.accentHover : p.accent)
                                      : p.fgDisabled);
    FillCircle(dc, x, cy, r - theme::Dp(3), p.bg);
}

void SliderRow::SetFromX(int x, const RECT& client)
{
    const RECT track = TrackRect(client);
    const int tw = track.right - track.left;
    if (tw <= 0)
        return;
    double pos = (double)(x - track.left) / (double)tw;
    pos = pos < 0.0 ? 0.0 : (pos > 1.0 ? 1.0 : pos);

    uint32_t v;
    if (_steps > 1)
        v = Canonical((uint32_t)(pos * (_steps - 1) + 0.5), _hi, _steps);
    else
        v = (uint32_t)(pos * _hi + 0.5);
    if (v > _hi) v = _hi;

    if (v != _stored)
    {
        _stored = v;
        Invalidate();
        if (onMove) onMove();
    }
}

void SliderRow::Step(int delta)
{
    uint32_t v;
    if (_steps > 1)
    {
        const int n = Clampi((int)DetentOf(_stored, _hi, _steps) + delta, 0, (int)_steps - 1);
        v = Canonical((uint32_t)n, _hi, _steps);
    }
    else
    {
        const int stepSize = (int)(_hi / 32) + 1;
        v = (uint32_t)Clampi((int)_stored + delta * stepSize, 0, (int)_hi);
    }
    if (v != _stored)
    {
        _stored = v;
        Invalidate();
        if (onMove) onMove();
        if (onCommit) onCommit();
    }
}

bool SliderRow::OnMouse(UINT msg, POINT pt)
{
    RECT client{};
    GetClientRect(_h, &client);

    switch (msg)
    {
    case WM_LBUTTONDBLCLK:
    case WM_LBUTTONDOWN:
        SetFocus(_h);
        _dragging = true;
        SetCapture(_h);
        SetFromX(pt.x, client);
        return true;

    case WM_MOUSEMOVE:
        if (_dragging)
        {
            SetFromX(pt.x, client);
            return true;
        }
        return false;

    case WM_LBUTTONUP:
        if (_dragging)
        {
            _dragging = false;
            ReleaseCapture();
            Invalidate();
            if (onCommit) onCommit();
            return true;
        }
        return false;

    case WM_MOUSEWHEEL:
        Step(pt.y > 0 ? 1 : -1);
        return true;
    }
    return false;
}

bool SliderRow::OnKey(WPARAM vk)
{
    switch (vk)
    {
    case VK_LEFT:  case VK_DOWN:  Step(-1); return true;
    case VK_RIGHT: case VK_UP:    Step(+1); return true;
    case VK_PRIOR: Step(+4); return true;
    case VK_NEXT:  Step(-4); return true;
    case VK_HOME:
    case VK_END:
    {
        const uint32_t v = (vk == VK_HOME) ? 0u : _hi;
        if (v != _stored)
        {
            _stored = v;
            Invalidate();
            if (onMove) onMove();
            if (onCommit) onCommit();
        }
        return true;
    }
    }
    return false;
}

// ---------------------------------------------------------------------------
// ToggleRow

bool ToggleRow::Create(HWND parent, const RECT& px, const wchar_t* label, Look look)
{
    _label = label ? label : L"";
    _look  = look;
    return Widget::Create(parent, px, true, _label.c_str());
}

void ToggleRow::SetChecked(bool on)
{
    if (_on == on)
        return;
    _on = on;
    Invalidate();
}

void ToggleRow::Flip()
{
    _on = !_on;
    Invalidate();
    if (onToggle) onToggle();
}

void ToggleRow::OnPaint(HDC dc, const RECT& client)
{
    const theme::Palette& p = theme::C();
    const COLORREF fg = _enabled ? p.fg : p.fgDisabled;
    const COLORREF on = _enabled ? p.accent : p.fgDisabled;

    RECT text = client;
    if (_look == Look::Check)
    {
        RECT box = layout::VCenter(client, kBoxSize);
        box.right = box.left + theme::Dp(kBoxSize);
        if (_on)
        {
            theme::FillRound(dc, box, 3, on);
            // Check glyph: two strokes, drawn as a polyline.
            HGDIOBJ op = SelectObject(dc, theme::Pen(p.bg, theme::Dp(2)));
            const int w = box.right - box.left, h = box.bottom - box.top;
            POINT pts[3] = {
                { box.left + w * 22 / 100, box.top + h * 52 / 100 },
                { box.left + w * 42 / 100, box.top + h * 72 / 100 },
                { box.left + w * 78 / 100, box.top + h * 28 / 100 },
            };
            Polyline(dc, pts, 3);
            SelectObject(dc, op);
        }
        else
        {
            theme::FrameRound(dc, box, 3, _enabled ? (_hot ? p.fgMuted : p.border)
                                                   : p.fgDisabled);
        }
        text.left = box.right + theme::Dp(kGap);
    }
    else
    {
        RECT sw = layout::VCenter(client, kSwitchH);
        sw.right = sw.left + theme::Dp(kSwitchW);
        theme::FillRound(dc, sw, kSwitchH / 2, _on ? on : p.bgInset);
        if (!_on)
            theme::FrameRound(dc, sw, kSwitchH / 2, _enabled ? p.border : p.fgDisabled);
        const int r  = theme::Dp(kSwitchH / 2 - 3);
        const int cy = (sw.top + sw.bottom) / 2;
        const int cx = _on ? sw.right - theme::Dp(kSwitchH / 2)
                           : sw.left  + theme::Dp(kSwitchH / 2);
        FillCircle(dc, cx, cy, r, _on ? p.bg : (_enabled ? p.fgMuted : p.fgDisabled));
        text.left = sw.right + theme::Dp(kGap);
    }

    if (_focused && _enabled)
    {
        RECT f = client;
        f.right = text.left - theme::Dp(kGap) / 2;
        theme::FrameRound(dc, f, 4, p.borderFocus);
    }

    theme::Text(dc, text, _label.c_str(), theme::Font::Body, fg,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

bool ToggleRow::OnMouse(UINT msg, POINT pt)
{
    switch (msg)
    {
    case WM_LBUTTONDOWN:
        SetFocus(_h);
        _pressed = true;
        SetCapture(_h);
        return true;

    // Swallowed, not treated as a second toggle. The class carries CS_DBLCLKS,
    // and a toggle whose handler is slow (split re-creates both devices) invites
    // an impatient second click -- which as a double-click would silently undo
    // the first. A double-click toggles once.
    case WM_LBUTTONDBLCLK:
        return true;

    case WM_LBUTTONUP:
        if (!_pressed)
            return false;
        _pressed = false;
        ReleaseCapture();
        {
            RECT c{};
            GetClientRect(_h, &c);
            if (PtInRect(&c, pt))
                Flip();
        }
        return true;
    }
    return false;
}

bool ToggleRow::OnKey(WPARAM vk)
{
    if (vk == VK_SPACE)
    {
        Flip();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// MeterBank

bool MeterBank::Create(HWND parent, const RECT& px)
{
    return Widget::Create(parent, px, false, L"Microphone levels");
}

bool MeterBank::SetLevels(const float* rms, int count, bool live)
{
    if (count > kMaxChannels) count = kMaxChannels;
    if (count < 0) count = 0;

    bool changed = (_count != count) || (_live != live);
    _count = count;
    _live  = live;

    const DWORD now = GetTickCount();
    for (int i = 0; i < kMaxChannels; ++i)
    {
        Ch& c = _ch[i];
        int bar = 0;
        if (live && i < count && rms && rms[i] > 0.0f)
        {
            // -60..0 dBFS across the well; below the floor reads as silence.
            const double db = 20.0 * log10((double)rms[i]);
            bar = Clampi((int)((db + 60.0) / 60.0 * 200.0 + 0.5), 0, 200);
            if (rms[i] >= 0.99f)
                c.clipUntil = now + 1000;
        }
        if (bar != c.bar) { c.bar = bar; changed = true; }

        const int decayed = c.peak > 7 ? c.peak - 7 : 0;
        const int peak = bar > decayed ? bar : decayed;
        if (peak != c.peak) { c.peak = peak; changed = true; }

        if (c.clipUntil && (int)(now - c.clipUntil) >= 0) { c.clipUntil = 0; changed = true; }
    }
    return changed;
}

void MeterBank::OnPaint(HDC dc, const RECT& client)
{
    const theme::Palette& p = theme::C();
    const int n = _count > 0 ? _count : kMaxChannels;
    const int gap = theme::Dp(3);
    const int total = client.bottom - client.top;
    const int hEach = (total - gap * (n - 1)) / (n ? n : 1);
    if (hEach <= 0)
        return;

    for (int i = 0; i < n; ++i)
    {
        RECT well{ client.left, client.top + i * (hEach + gap),
                   client.right, client.top + i * (hEach + gap) + hEach };
        theme::FillRound(dc, well, 2, p.bgInset);

        const int w = well.right - well.left;
        if (w <= 0) continue;

        const Ch& c = _ch[i];
        if (c.bar > 0)
        {
            RECT bar = well;
            bar.right = well.left + c.bar * w / 200;
            const bool clip = c.clipUntil != 0;
            theme::FillRound(dc, bar, 2, clip ? p.bad : (_live ? p.good : p.fgDisabled));
        }
        if (c.peak > 0)
        {
            const int px = well.left + Clampi(c.peak * w / 200, 0, w - 1);
            RECT tick{ px, well.top, px + theme::Dp(2), well.bottom };
            theme::Fill(dc, tick, _live ? p.fg : p.fgDisabled);
        }
    }
}

// ---------------------------------------------------------------------------
// Button

bool Button::Create(HWND parent, const RECT& px, const wchar_t* text, Kind kind)
{
    _text = text ? text : L"";
    _kind = kind;
    return Widget::Create(parent, px, true, _text.c_str());
}

void Button::SetText(const wchar_t* text)
{
    if (_text == text)
        return;
    _text = text ? text : L"";
    SetWindowTextW(_h, _text.c_str());
    Invalidate();
}

void Button::OnPaint(HDC dc, const RECT& client)
{
    const theme::Palette& p = theme::C();
    RECT r = client;

    if (_kind == Kind::Primary)
    {
        const COLORREF fill = !_enabled ? p.bgInset
                            : _pressed  ? p.accentPressed
                            : _hot      ? p.accentHover : p.accent;
        theme::FillRound(dc, r, 4, fill);
        theme::Text(dc, r, _text.c_str(), theme::Font::Strong,
                    _enabled ? p.bg : p.fgDisabled,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    else
    {
        if (_hot && _enabled)
            theme::FillRound(dc, r, 4, p.bgElevated);
        theme::FrameRound(dc, r, 4, _enabled ? (_hot ? p.fgMuted : p.border) : p.bgInset);
        theme::Text(dc, r, _text.c_str(), theme::Font::Body,
                    _enabled ? p.fg : p.fgDisabled,
                    DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    if (_focused && _enabled)
        theme::FrameRound(dc, layout::Inset(r, 2, 2), 3, p.borderFocus);
}

bool Button::OnMouse(UINT msg, POINT pt)
{
    // Swallowed rather than treated as a fresh press: Record would otherwise
    // start and immediately stop on a double-click.
    if (msg == WM_LBUTTONDBLCLK)
        return true;
    if (msg == WM_LBUTTONDOWN)
    {
        SetFocus(_h);
        _pressed = true;
        SetCapture(_h);
        Invalidate();
        return true;
    }
    if (msg == WM_LBUTTONUP && _pressed)
    {
        _pressed = false;
        ReleaseCapture();
        Invalidate();
        RECT c{};
        GetClientRect(_h, &c);
        if (PtInRect(&c, pt) && onClick)
            onClick();
        return true;
    }
    return false;
}

bool Button::OnKey(WPARAM vk)
{
    if (vk == VK_SPACE || vk == VK_RETURN)
    {
        if (onClick) onClick();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// SectionHeader

bool SectionHeader::Create(HWND parent, const RECT& px, const wchar_t* text)
{
    _text = text ? text : L"";
    return Widget::Create(parent, px, false, _text.c_str());
}

void SectionHeader::OnPaint(HDC dc, const RECT& client)
{
    const theme::Palette& p = theme::C();
    const int w = theme::TextWidth(dc, _text.c_str(), theme::Font::Caps);
    theme::Text(dc, client, _text.c_str(), theme::Font::Caps, p.fgMuted,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    const int y = (client.top + client.bottom) / 2;
    RECT rule{ client.left + w + theme::Dp(8), y, client.right, y + theme::Dp(1) };
    if (rule.right > rule.left)
        theme::Fill(dc, rule, p.border);
}

// ---------------------------------------------------------------------------
// InfoText

bool InfoText::Create(HWND parent, const RECT& px)
{
    return Widget::Create(parent, px, false);
}

bool InfoText::SetText(const wchar_t* text)
{
    const wchar_t* t = text ? text : L"";
    if (_text == t)
        return false;
    _text = t;
    SetWindowTextW(_h, _text.c_str());
    Invalidate();
    return true;
}

void InfoText::SetLink(bool on)
{
    if (_link == on)
        return;
    _link = on;
    Invalidate();
}

void InfoText::OnPaint(HDC dc, const RECT& client)
{
    const theme::Palette& p = theme::C();
    COLORREF c = p.fgMuted;
    if (_link)
        c = _hot ? p.accentHover : p.accent;
    const UINT align = _centered ? DT_CENTER : DT_LEFT;
    theme::Text(dc, client, _text.c_str(), _font, c,
                _single ? (align | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS)
                        : (align | DT_WORDBREAK | DT_TOP));
}

bool InfoText::OnSetCursor()
{
    if (!_link)
        return false;
    SetCursor(LoadCursorW(nullptr, IDC_HAND));
    return true;
}

bool InfoText::OnMouse(UINT msg, POINT pt)
{
    (void)pt;
    if (msg == WM_LBUTTONDOWN && _link && onClick)
    {
        onClick();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// StatusPill

bool StatusPill::Create(HWND parent, const RECT& px)
{
    return Widget::Create(parent, px, false);
}

bool StatusPill::Set(COLORREF dot, const wchar_t* text)
{
    const wchar_t* t = text ? text : L"";
    if (_dot == dot && _text == t)
        return false;
    _dot  = dot;
    _text = t;
    SetWindowTextW(_h, _text.c_str());
    Invalidate();
    return true;
}

void StatusPill::OnPaint(HDC dc, const RECT& client)
{
    const theme::Palette& p = theme::C();
    const int r  = theme::Dp(kDotR);
    const int cy = (client.top + client.bottom) / 2;
    FillCircle(dc, client.left + r, cy, r, _dot);
    RECT t = client;
    t.left += r * 2 + theme::Dp(7);
    theme::Text(dc, t, _text.c_str(), theme::Font::Body, p.fg,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

// ---------------------------------------------------------------------------
// Chip

bool Chip::Create(HWND parent, const RECT& px, const wchar_t* text)
{
    _text = text ? text : L"";
    return Widget::Create(parent, px, true, _text.c_str());
}

void Chip::SetText(const wchar_t* text)
{
    const wchar_t* t = text ? text : L"";
    if (_text == t)
        return;
    _text = t;
    SetWindowTextW(_h, _text.c_str());
    Invalidate();
}

void Chip::SetSelected(bool on)
{
    if (_selected == on)
        return;
    _selected = on;
    Invalidate();
}

void Chip::SetDot(COLORREF c)
{
    if (_dot == c)
        return;
    _dot = c;
    Invalidate();
}

int Chip::MeasureWidth(HDC dc) const
{
    return theme::TextWidth(dc, _text.c_str(), theme::Font::Body) +
           theme::Dp(kDotR * 2 + 10 + 14 + 14);
}

void Chip::OnPaint(HDC dc, const RECT& client)
{
    const theme::Palette& p = theme::C();
    if (_selected)
        theme::FillRound(dc, client, 6, p.bgElevated);
    else if (_hot)
        theme::FillRound(dc, client, 6, p.bgInset);
    theme::FrameRound(dc, client, 6, _selected ? p.accent : p.border);
    if (_focused)
        theme::FrameRound(dc, layout::Inset(client, 2, 2), 5, p.borderFocus);

    const int r  = theme::Dp(kDotR);
    const int cy = (client.top + client.bottom) / 2;
    FillCircle(dc, client.left + theme::Dp(12), cy, r, _dot);
    RECT t = client;
    t.left += theme::Dp(12) + r + theme::Dp(9);
    t.right -= theme::Dp(10);
    theme::Text(dc, t, _text.c_str(), theme::Font::Body,
                _selected ? p.fg : p.fgMuted,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

bool Chip::OnMouse(UINT msg, POINT pt)
{
    (void)pt;
    if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
    {
        SetFocus(_h);
        if (onClick) onClick();
        return true;
    }
    return false;
}

bool Chip::OnKey(WPARAM vk)
{
    if (vk == VK_SPACE || vk == VK_RETURN)
    {
        if (onClick) onClick();
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Combo

bool Combo::Create(HWND parent, const RECT& px, int id)
{
    _h = CreateWindowExW(0, L"COMBOBOX", L"",
                         WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL |
                         CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                         px.left, px.top, px.right - px.left, px.bottom - px.top,
                         parent, (HMENU)(INT_PTR)id,
                         (HINSTANCE)GetWindowLongPtrW(parent, GWLP_HINSTANCE), nullptr);
    if (!_h)
        return false;
    Restyle();
    return true;
}

void Combo::Restyle()
{
    if (!_h)
        return;
    SendMessageW(_h, WM_SETFONT, (WPARAM)theme::GetFont(theme::Font::Body), TRUE);
    // Best-effort: darkens the frame and drop-down on builds that know the
    // class. Ignored elsewhere, which costs only a light frame.
    SetWindowTheme(_h, theme::IsDark() ? L"DarkMode_CFD" : L"Explorer", nullptr);
    SendMessageW(_h, CB_SETITEMHEIGHT, (WPARAM)-1, ItemHeightPx());
}

void Combo::Move(const RECT& px)
{
    if (!_h) return;
    // The height passed here is the CLOSED height; the list sizes itself.
    SetWindowPos(_h, nullptr, px.left, px.top, px.right - px.left, px.bottom - px.top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
}

void Combo::Show(bool on)   { if (_h) ShowWindow(_h, on ? SW_SHOW : SW_HIDE); }
void Combo::Enable(bool on) { if (_h) EnableWindow(_h, on); }
void Combo::Clear()         { if (_h) SendMessageW(_h, CB_RESETCONTENT, 0, 0); }

int Combo::Add(const wchar_t* text, LPARAM data)
{
    if (!_h) return -1;
    const int i = (int)SendMessageW(_h, CB_ADDSTRING, 0, (LPARAM)text);
    if (i >= 0)
        SendMessageW(_h, CB_SETITEMDATA, i, data);
    return i;
}

void Combo::SetCurSel(int i) { if (_h) SendMessageW(_h, CB_SETCURSEL, i, 0); }
int  Combo::CurSel() const   { return _h ? (int)SendMessageW(_h, CB_GETCURSEL, 0, 0) : -1; }
LPARAM Combo::Data(int i) const
{
    return _h ? (LPARAM)SendMessageW(_h, CB_GETITEMDATA, i, 0) : 0;
}
void Combo::SetDroppedWidthPx(int px)
{
    if (_h) SendMessageW(_h, CB_SETDROPPEDWIDTH, (WPARAM)px, 0);
}

int Combo::ItemHeightPx() { return theme::Dp(20); }

void Combo::DrawItem(const DRAWITEMSTRUCT* d)
{
    if (!d || d->itemID == (UINT)-1)
        return;
    const theme::Palette& p = theme::C();
    const bool sel = (d->itemState & ODS_SELECTED) != 0;
    const bool dis = (d->itemState & ODS_DISABLED) != 0;

    theme::Fill(d->hDC, d->rcItem, sel ? p.accent : p.bgElevated);

    wchar_t text[512] = {};
    SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, (LPARAM)text);
    RECT t = d->rcItem;
    t.left += theme::Dp(8);
    t.right -= theme::Dp(4);
    theme::Text(d->hDC, t, text, theme::Font::Body,
                dis ? p.fgDisabled : (sel ? p.bg : p.fg),
                DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
}

} // namespace ui
