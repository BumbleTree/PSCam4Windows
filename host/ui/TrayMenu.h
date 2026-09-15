#pragma once
//
// TrayMenu — the tray context menu, built from the live slot map and drawn dark.
//
// Every connected camera gets its own submenu, so the menu drives the whole hub
// rather than slot 0. Command ids are IDM_CAM_BASE + slot * kPerSlot + item;
// Decode() turns a command back into (slot, item).
//
// Win32 popup menus follow no app theme, so items are owner-drawn. The owner is
// the tray window, which forwards WM_MEASUREITEM / WM_DRAWITEM here.
//
#include <windows.h>

class CaptureController;

namespace traymenu {

// Items within one camera's block. Modes occupy kModeBase upward.
enum Item : int
{
    ItemFlipH = 0,
    ItemFlipV = 1,
    ItemAutoGain = 2,
    ItemModeBase = 8,
};
constexpr int kPerSlot = 32;      // items reserved per camera

struct Command { int slot; int item; };

// True when `id` belongs to a camera block, with the decoded slot and item.
bool Decode(int id, Command& out);

// Build and track the menu at `anchor`. Returns the chosen command id, or 0.
// `owner` receives the owner-draw messages and must forward them to
// MeasureItem / DrawItem below.
int Track(HWND owner, POINT anchor, CaptureController* controllers);

void MeasureItem(MEASUREITEMSTRUCT* mis);
void DrawItem(const DRAWITEMSTRUCT* dis);

// Frees the strings owner-draw items carry. Called after tracking.
void Cleanup();

} // namespace traymenu
