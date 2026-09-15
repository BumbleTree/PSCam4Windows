#include "TrayMenu.h"

#include <string>
#include <vector>

#include "Theme.h"
#include "../Autostart.h"
#include "../CaptureController.h"
#include "../DeviceRegistry.h"
#include "../../common/Settings.h"
#include "../../common/VCamGuids.h"
#include "../../res/resource.h"

namespace traymenu {
namespace {

// One owner-drawn item. The menu holds pointers to these, so they outlive
// TrackPopupMenuEx and are released by Cleanup().
struct Entry
{
    std::wstring text;
    bool checked = false;
    bool header  = false;   // a camera name: bold, not selectable
    bool sep     = false;
};

std::vector<Entry*> g_entries;

Entry* NewEntry(std::wstring text, bool checked = false, bool header = false)
{
    Entry* e = new Entry{ std::move(text), checked, header, false };
    g_entries.push_back(e);
    return e;
}

void Append(HMENU m, UINT flags, UINT_PTR id, Entry* e)
{
    MENUITEMINFOW mi{ sizeof(mi) };
    mi.fMask      = MIIM_FTYPE | MIIM_ID | MIIM_DATA | MIIM_STATE;
    mi.fType      = MFT_OWNERDRAW;
    mi.wID        = (UINT)id;
    mi.dwItemData = (ULONG_PTR)e;
    mi.fState     = (flags & MF_DISABLED) ? MFS_DISABLED : MFS_ENABLED;
    if (flags & MF_POPUP)
    {
        mi.fMask |= MIIM_SUBMENU;
        mi.hSubMenu = (HMENU)id;
        mi.wID = 0;
    }
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}

void AppendSeparator(HMENU m)
{
    MENUITEMINFOW mi{ sizeof(mi) };
    mi.fMask = MIIM_FTYPE;
    mi.fType = MFT_SEPARATOR;
    InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
}

} // namespace

bool Decode(int id, Command& out)
{
    if (id < IDM_CAM_BASE || id >= IDM_CAM_BASE + kVCamCount * kPerSlot)
        return false;
    const int rel = id - IDM_CAM_BASE;
    out.slot = rel / kPerSlot;
    out.item = rel % kPerSlot;
    return true;
}

int Track(HWND owner, POINT anchor, CaptureController* controllers)
{
    Cleanup();
    HMENU root = CreatePopupMenu();

    Append(root, MF_STRING, IDM_SETTINGS, NewEntry(L"Settings…"));
    SetMenuDefaultItem(root, IDM_SETTINGS, FALSE);

    int cameras = 0;
    for (int slot = 0; slot < kVCamCount; ++slot)
    {
        if (!controllers ||
            controllers[slot].GetState() == CaptureController::State::CameraMissing)
            continue;

        const DeviceProfile* prof = deviceregistry::ProfileForSlot(slot);
        if (!prof)
            continue;

        if (cameras++ == 0)
            AppendSeparator(root);

        wchar_t name[64];
        deviceregistry::SlotDisplayName(slot, name, 64);

        HMENU cam = CreatePopupMenu();
        const Settings s = settings::Load(slot);
        const int base = IDM_CAM_BASE + slot * kPerSlot;

        HMENU modes = CreatePopupMenu();
        const int modeCount = (int)prof->modeCount;
        for (int i = 0; i < modeCount && i < kPerSlot - ItemModeBase; ++i)
        {
            wchar_t item[48];
            swprintf_s(item, L"%u × %u @ %u fps",
                       prof->modes[i].width, prof->modes[i].height, prof->modes[i].fps);
            const bool on = prof->modes[i].width == s.width &&
                            prof->modes[i].height == s.height &&
                            prof->modes[i].fps == s.fps;
            Append(modes, MF_STRING, base + ItemModeBase + i, NewEntry(item, on));
        }
        Append(cam, MF_POPUP, (UINT_PTR)modes, NewEntry(L"Video mode"));

        if (prof->controlMask & CTRL_FLIP)
        {
            Append(cam, MF_STRING, base + ItemFlipH, NewEntry(L"Flip horizontally", s.flipH));
            Append(cam, MF_STRING, base + ItemFlipV, NewEntry(L"Flip vertically",   s.flipV));
        }
        if (prof->controlMask & CTRL_GAIN)
            Append(cam, MF_STRING, base + ItemAutoGain, NewEntry(L"Auto gain", s.autoGain));

        Append(root, MF_POPUP, (UINT_PTR)cam, NewEntry(name, false, true));
    }

    if (cameras == 0)
    {
        AppendSeparator(root);
        Append(root, MF_STRING | MF_DISABLED, 0, NewEntry(L"No camera connected", false, true));
    }

    AppendSeparator(root);
    Append(root, MF_STRING, IDM_AUTOSTART,
           NewEntry(L"Start with Windows", autostart::IsEnabled()));
    AppendSeparator(root);
    Append(root, MF_STRING, IDM_EXIT, NewEntry(L"Exit"));

    // Foreground first, or the menu will not dismiss on an outside click.
    SetForegroundWindow(owner);
    const int chosen = (int)TrackPopupMenuEx(root, TPM_RIGHTBUTTON | TPM_RETURNCMD,
                                             anchor.x, anchor.y, owner, nullptr);
    PostMessageW(owner, WM_NULL, 0, 0);
    DestroyMenu(root);
    return chosen;
}

void MeasureItem(MEASUREITEMSTRUCT* mis)
{
    if (!mis || mis->CtlType != ODT_MENU)
        return;
    const Entry* e = (const Entry*)mis->itemData;
    HDC dc = GetDC(nullptr);
    const int w = e ? theme::TextWidth(dc, e->text.c_str(),
                                       e->header ? theme::Font::Strong : theme::Font::Body)
                    : 0;
    ReleaseDC(nullptr, dc);
    mis->itemWidth  = w + theme::Dp(46);
    mis->itemHeight = theme::Dp(24);
}

void DrawItem(const DRAWITEMSTRUCT* dis)
{
    if (!dis || dis->CtlType != ODT_MENU)
        return;
    const Entry* e = (const Entry*)dis->itemData;
    if (!e)
        return;

    const theme::Palette& p = theme::C();
    const bool sel = (dis->itemState & (ODS_SELECTED | ODS_HOTLIGHT)) != 0;
    const bool dis_ = (dis->itemState & (ODS_DISABLED | ODS_GRAYED)) != 0;

    theme::Fill(dis->hDC, dis->rcItem, sel && !dis_ ? p.accent : p.bgElevated);

    RECT t = dis->rcItem;
    t.left  += theme::Dp(30);
    t.right -= theme::Dp(12);
    const COLORREF fg = dis_ ? p.fgDisabled : (sel ? p.bg : p.fg);
    theme::Text(dis->hDC, t, e->text.c_str(),
                e->header ? theme::Font::Strong : theme::Font::Body, fg,
                DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (e->checked)
    {
        // A check mark drawn rather than themed: DrawFrameControl has no dark
        // form, and this is the same glyph the toggle widget uses.
        RECT b = dis->rcItem;
        b.left += theme::Dp(11);
        const int cy = (b.top + b.bottom) / 2;
        HGDIOBJ op = SelectObject(dis->hDC, theme::Pen(fg, theme::Dp(2)));
        POINT pts[3] = {
            { b.left,                  cy + theme::Dp(0) },
            { b.left + theme::Dp(4),   cy + theme::Dp(4) },
            { b.left + theme::Dp(11),  cy - theme::Dp(5) },
        };
        Polyline(dis->hDC, pts, 3);
        SelectObject(dis->hDC, op);
    }
}

void Cleanup()
{
    for (Entry* e : g_entries)
        delete e;
    g_entries.clear();
}

} // namespace traymenu
