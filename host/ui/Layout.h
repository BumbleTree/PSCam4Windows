#pragma once
//
// Layout — a stack solver over device-independent pixels.
//
// A column emits cells top to bottom. A control that is not shown is never
// emitted, so a hidden row leaves no gap: there is no second pass that closes
// one. All constants are DIP; cells come back in physical pixels for the
// current DPI, so a DPI change is a relayout rather than a rescale.
//
#include <windows.h>
#include "Theme.h"

namespace layout {

inline RECT Px(int xDip, int yDip, int wDip, int hDip)
{
    const int x = theme::Dp(xDip), y = theme::Dp(yDip);
    return { x, y, x + theme::Dp(wDip), y + theme::Dp(hDip) };
}

class Column
{
public:
    Column(int xDip, int yDip, int widthDip, int gapDip)
        : _x(xDip), _y(yDip), _w(widthDip), _gap(gapDip) {}

    // The next full-width cell, `hDip` tall.
    RECT Next(int hDip)
    {
        if (_any) _cursor += _gap;
        _any = true;
        RECT r = Px(_x, _y + _cursor, _w, hDip);
        _cursor += hDip;
        return r;
    }

    void Gap(int dip)   { _cursor += dip; }
    int  XDip() const     { return _x; }
    int  WidthDip() const { return _w; }
    int  BottomDip() const { return _y + _cursor; }
    bool Empty() const  { return !_any; }

private:
    int  _x, _y, _w, _gap;
    int  _cursor = 0;
    bool _any = false;
};

// Carve a row into label | body | value. Any of the three may be zero-width.
struct Cells { RECT label, body, value; };

inline Cells Split(const RECT& r, int labelDip, int valueDip, int gapDip)
{
    const int lab = theme::Dp(labelDip);
    const int val = theme::Dp(valueDip);
    const int gap = theme::Dp(gapDip);
    Cells c{};
    c.label = { r.left, r.top, r.left + lab, r.bottom };
    c.value = { r.right - val, r.top, r.right, r.bottom };
    c.body  = { c.label.right + (lab ? gap : 0), r.top,
                c.value.left  - (val ? gap : 0), r.bottom };
    return c;
}

// Vertically centre a `hDip`-tall band inside `r`.
inline RECT VCenter(const RECT& r, int hDip)
{
    const int h = theme::Dp(hDip);
    const int y = r.top + ((r.bottom - r.top) - h) / 2;
    return { r.left, y, r.right, y + h };
}

inline RECT Inset(const RECT& r, int dxDip, int dyDip)
{
    const int dx = theme::Dp(dxDip), dy = theme::Dp(dyDip);
    return { r.left + dx, r.top + dy, r.right - dx, r.bottom - dy };
}

} // namespace layout
