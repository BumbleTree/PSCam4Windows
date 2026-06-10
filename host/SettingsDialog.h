#pragma once
//
// Modeless settings dialog (single instance). Lives on the UI thread.
//
#include <windows.h>
#include "CaptureController.h"

class TrayUI;

namespace settingsdialog
{
    // Must be called once before Show (settings changes route through the
    // tray so balloon/persist logic lives in one place).
    void SetTray(TrayUI* tray);

    // Creates the dialog or brings the existing one to the foreground.
    void Show(HINSTANCE instance, CaptureController* controller);

    // Refresh the status line / checkbox enables after a controller state
    // change or external settings change. No-op when the dialog is closed.
    void RefreshStatus();

    HWND Hwnd();
    void Close();
}
