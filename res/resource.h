#pragma once

// Icons
#define IDI_APP             101

// The settings window. The template is an empty shell -- every control is
// created in code by host/ui/SettingsWindow.cpp -- so only the few children
// that must be addressed by id appear here.
#define IDD_SETTINGS        200
#define IDC_PREVIEW         230

// Combo boxes, which stay native and so report through WM_COMMAND.
#define IDC_MODECOMBO       201
#define IDC_VIEWCOMBO       202
#define IDC_FLICKCOMBO      203
#define IDC_MICOUTCOMBO     204
#define IDC_THEMECOMBO      205

// Tray menu
#define IDM_SETTINGS        300
#define IDM_AUTOSTART       301
#define IDM_EXIT            305
// Per-camera commands: IDM_CAM_BASE + slot * kTrayPerSlot + item, so every
// connected camera drives its own submenu rather than only slot 0.
#define IDM_CAM_BASE        400
