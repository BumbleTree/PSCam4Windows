#include "TrayUI.h"

#include <commctrl.h>
#include <shellapi.h>
#include <windowsx.h>
#include <dbt.h>
#include <cstdio>

#include "SettingsDialog.h"
#include "Autostart.h"
#include "DeviceRegistry.h"
#include "MicWatch.h"
#include "ui/TrayMenu.h"
#include "../common/VCamGuids.h"
#include "../res/resource.h"

namespace
{
constexpr UINT kTrayIconId = 1;
}

bool TrayUI::Create(HINSTANCE instance, CaptureController* controllers)
{
    _instance = instance;
    _controllers = controllers;
    _controller = &controllers[0];

    LoadIconMetric(instance, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &_icon);

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProcThunk;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    RegisterClassW(&wc);

    // A normal (never-shown) top-level window, NOT a message-only window:
    // broadcast messages like TaskbarCreated are only delivered to top-level
    // windows.
    _hwnd = CreateWindowExW(0, kWindowClass, L"PSCam4Win Virtual Camera",
                            WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!_hwnd)
        return false;

    // Explorer (medium IL) broadcasts TaskbarCreated; without this filter the
    // elevated process never sees it and the icon dies on Explorer restarts.
    _taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(_hwnd, _taskbarCreatedMsg, MSGFLT_ALLOW, nullptr);
    ChangeWindowMessageFilterEx(_hwnd, WM_SHOW_SETTINGS, MSGFLT_ALLOW, nullptr);

    // Subscribe to each camera interface (PS3 Eye + EyeToy) arrival/removal so
    // the capture threads can re-evaluate slot occupancy immediately instead of
    // polling. One registration per GUID; DBT_DEVICEARRIVAL/REMOVECOMPLETE for
    // any of them lands in the same WndProc handler.
    for (int i = 0; i < kCameraInterfaceGuidCount; ++i)
    {
        DEV_BROADCAST_DEVICEINTERFACE_W filter{};
        filter.dbcc_size = sizeof(filter);
        filter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
        filter.dbcc_classguid = kCameraInterfaceGuids[i];
        _devNotify[i] = RegisterDeviceNotificationW(_hwnd, &filter, DEVICE_NOTIFY_WINDOW_HANDLE);
    }

    AddTrayIcon();

    // A camera wedged before we started is the case prevention cannot reach —
    // the state was left on the chip by an earlier session, and only a replug
    // clears it. Ask once, after the shell and usbaudio have settled.
    SetTimer(_hwnd, 2, 8000, nullptr);
    return true;
}

void TrayUI::Destroy()
{
    micwatch::Shutdown();   // the probe posts to _hwnd; join it before it goes
    RemoveTrayIcon();
    for (int i = 0; i < kCameraInterfaceGuidCount; ++i)
    {
        if (_devNotify[i])
        {
            UnregisterDeviceNotification(_devNotify[i]);
            _devNotify[i] = nullptr;
        }
    }
    if (_hwnd)
    {
        DestroyWindow(_hwnd);
        _hwnd = nullptr;
    }
}

void TrayUI::AddTrayIcon()
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    nid.uCallbackMessage = WM_TRAY;
    nid.hIcon = _icon;
    wcscpy_s(nid.szTip, L"PSCam4Win Virtual Camera");
    Shell_NotifyIconW(_iconAdded ? NIM_MODIFY : NIM_ADD, &nid);
    if (!_iconAdded)
    {
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
        _iconAdded = true;
    }
    UpdateTooltip();
}

void TrayUI::RemoveTrayIcon()
{
    if (!_iconAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    _iconAdded = false;
}

void TrayUI::UpdateTooltip()
{
    if (!_iconAdded)
        return;
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_TIP | NIF_SHOWTIP;

    int streamingCount = 0;
    int asleepCount = 0;
    int failedCount = 0;
    int replugCount = 0;
    float maxFps = 0.0f;

    for (int i = 0; i < kVCamCount; ++i)
    {
        switch (_controllers[i].GetState())
        {
        case CaptureController::State::Streaming:
            streamingCount++;
            if (_controllers[i].MeasuredFpsX10() / 10.0f > maxFps)
                maxFps = _controllers[i].MeasuredFpsX10() / 10.0f;
            break;
        case CaptureController::State::Asleep:
            asleepCount++;
            break;
        case CaptureController::State::VCamFailed:
            failedCount++;
            break;
        case CaptureController::State::Ps4NeedsReplug:
            replugCount++;
            break;
        default:
            break;
        }
    }

    if (streamingCount > 0)
    {
        swprintf_s(nid.szTip, L"PSCam4Win — %d streaming (max %.1f fps), %d idle",
                   streamingCount, maxFps, asleepCount);
    }
    else if (replugCount > 0)
    {
        wcscpy_s(nid.szTip, L"PSCam4Win — PS4 camera needs a replug");
    }
    else if (failedCount > 0)
    {
        wcscpy_s(nid.szTip, L"PSCam4Win — virtual camera error (retrying)");
    }
    else if (asleepCount > 0)
    {
        swprintf_s(nid.szTip, L"PSCam4Win — %d idle (camera sleeping)", asleepCount);
    }
    else
    {
        wcscpy_s(nid.szTip, L"PSCam4Win — no cameras detected");
    }

    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayUI::ShowBalloon(const wchar_t* title, const wchar_t* text)
{
    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = _hwnd;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO | NIIF_RESPECT_QUIET_TIME;
    // _TRUNCATE, not wcscpy_s: these fields are 64 and 256 wide, and wcscpy_s
    // ABORTS the process on overflow rather than truncating. A balloon whose
    // wording grew past the limit must lose its tail, not take the tray down.
    wcsncpy_s(nid.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(nid.szInfo, text, _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

void TrayUI::ApplySettings(int cameraIndex, const Settings& s, bool persistNow)
{
    if (cameraIndex < 0 || cameraIndex >= kVCamCount)
        return;
    _controllers[cameraIndex].UpdateSettings(s);
    if (persistNow)
        settings::Save(cameraIndex, s);

    // The balloon only makes sense when the mode change is genuinely deferred
    // behind a live external client. When the preview is the only consumer
    // (or the camera isn't streaming), the mode applies immediately on the
    // next sleep transition — no need to bother the user.
    if (_controllers[cameraIndex].GetState() == CaptureController::State::Streaming &&
        !s.SameMode(_controllers[cameraIndex].ActiveSettings()) &&
        !_controllers[cameraIndex].IsPreviewOnly())
    {
        ShowBalloon(L"Mode change queued",
                    L"The new video mode will apply when no app is using the camera.");
    }
}

void TrayUI::RescanAllControllers()
{
    if (!_controllers)
        return;
    // Drop the cached slot map FIRST, exactly as the PnP path does.
    //
    // Callers reach here after changing something the map is COMPUTED FROM --
    // a PS4 Split or View setting -- and the registry cannot see a registry
    // write, so without this every query is served from a map up to its 2 s TTL
    // old. The settings dialog then repaints from that stale map: the Split
    // checkbox appears not to take for a second or two, and mid-unsplit both
    // slots still look like un-split PS4 cameras, so the duplicate-name rule
    // suffixes them ("PS4 Camera #5") until the TTL expires.
    deviceregistry::Invalidate();
    for (int i = 0; i < kVCamCount; ++i)
        _controllers[i].NotifyDeviceChange();
}

void TrayUI::ShowContextMenu(POINT anchor)
{
    const int chosen = traymenu::Track(_hwnd, anchor, _controllers);
    traymenu::Cleanup();
    if (chosen)
        OnCommand(chosen);
}

void TrayUI::OnCommand(int id)
{
    traymenu::Command cmd{};
    if (traymenu::Decode(id, cmd))
    {
        // Re-resolved rather than cached: the camera can be unplugged between
        // the menu opening and the click landing.
        const DeviceProfile* prof = deviceregistry::ProfileForSlot(cmd.slot);
        if (!prof)
            return;
        Settings s = settings::Load(cmd.slot);

        if (cmd.item >= traymenu::ItemModeBase)
        {
            const int i = cmd.item - traymenu::ItemModeBase;
            if (i >= static_cast<int>(prof->modeCount))
                return;
            s.width  = prof->modes[i].width;
            s.height = prof->modes[i].height;
            s.fps    = prof->modes[i].fps;
        }
        else
        {
            switch (cmd.item)
            {
            case traymenu::ItemFlipH:    s.flipH    = !s.flipH;    break;
            case traymenu::ItemFlipV:    s.flipV    = !s.flipV;    break;
            case traymenu::ItemAutoGain: s.autoGain = !s.autoGain; break;
            default: return;
            }
        }
        ApplySettings(cmd.slot, s, true);
        settingsdialog::RefreshStatus();
        return;
    }

    switch (id)
    {
    case IDM_SETTINGS:
        settingsdialog::Show(_instance, _controller);
        break;
    case IDM_AUTOSTART:
        if (autostart::IsEnabled())
            autostart::Disable();
        else if (!autostart::Enable())
            ShowBalloon(L"PSCam4Win", L"Could not update the scheduled task.");
        break;
    case IDM_EXIT:
        PostQuitMessage(0);
        break;
    }
}

LRESULT CALLBACK TrayUI::WndProcThunk(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    TrayUI* self;
    if (msg == WM_NCCREATE)
    {
        self = static_cast<TrayUI*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->_hwnd = hwnd;
    }
    else
    {
        self = reinterpret_cast<TrayUI*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->WndProc(hwnd, msg, wParam, lParam)
                : DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT TrayUI::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == _taskbarCreatedMsg && _taskbarCreatedMsg != 0)
    {
        _iconAdded = false;  // Explorer restarted: the icon is gone, re-add
        AddTrayIcon();
        return 0;
    }

    switch (msg)
    {
    // The menu is owner-drawn (Win32 popups follow no app theme) and tracked
    // with TPM_RETURNCMD, so selections come back from Track rather than as
    // WM_COMMAND; only the drawing messages arrive here.
    case WM_MEASUREITEM:
        traymenu::MeasureItem(reinterpret_cast<MEASUREITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_DRAWITEM:
        traymenu::DrawItem(reinterpret_cast<const DRAWITEMSTRUCT*>(lParam));
        return TRUE;

    case WM_TRAY:
        switch (LOWORD(lParam))
        {
        case NIN_SELECT:
        case NIN_KEYSELECT:
        case WM_LBUTTONDBLCLK:
            settingsdialog::Show(_instance, _controller);
            break;
        case WM_CONTEXTMENU:
        {
            POINT pt{ GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam) };
            ShowContextMenu(pt);
            break;
        }
        }
        return 0;

    case WM_DEVICECHANGE:
        if ((wParam == DBT_DEVICEARRIVAL || wParam == DBT_DEVICEREMOVECOMPLETE) && lParam)
        {
            const auto* hdr = reinterpret_cast<const DEV_BROADCAST_HDR*>(lParam);
            if (hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE && _controllers)
            {
                // Drop the cached slot map BEFORE waking the controllers, so the
                // first one to query re-enumerates and the rest reuse that result.
                deviceregistry::Invalidate();
                for (int i = 0; i < kVCamCount; ++i)
                    _controllers[i].NotifyDeviceChange();
                // A replug is exactly what cures the microphone wedge, so a
                // camera that just arrived deserves a fresh answer. Deferred:
                // usbaudio has not built the endpoint yet at DBT_DEVICEARRIVAL,
                // and probing now would just read Unknown.
                micwatch::Reset();
                SetTimer(hwnd, 2, 4000, nullptr);
            }
        }
        return TRUE;

    case WM_CONTROLLER_STATE:
        UpdateTooltip();
        // Arm the fps refresh only while something is streaming.
        {
            bool anyStreaming = false;
            for (int i = 0; i < kVCamCount && !anyStreaming; ++i)
                anyStreaming = (_controllers[i].GetState() == CaptureController::State::Streaming);
            if (anyStreaming && !_fpsTimerOn)
            {
                SetTimer(hwnd, 1, 3000, nullptr);
                _fpsTimerOn = true;
            }
            else if (!anyStreaming && _fpsTimerOn)
            {
                KillTimer(hwnd, 1);
                _fpsTimerOn = false;
            }
        }
        settingsdialog::RefreshStatus();
        // A state change is also how a plug/unplug surfaces (a slot enters or
        // leaves CameraMissing): keep the Settings dropdown in sync. Internally
        // a cheap no-op unless the set of connected cameras actually changed.
        settingsdialog::RefreshCameraList();
        if (static_cast<CaptureController::State>(wParam) == CaptureController::State::Fatal)
        {
            ShowBalloon(L"PSCam4Win",
                        L"A fatal error occurred — the virtual camera is not available.");
        }
        else if (static_cast<CaptureController::State>(wParam) == CaptureController::State::Ps4NeedsReplug)
        {
            // SetState posts only on a state change, so this fires once per wedged
            // episode rather than on every retry.
            ShowBalloon(L"PS4 Camera — please replug",
                        L"The PS4 camera needs to be unplugged and plugged back in to "
                        L"reload its firmware. This happens after the app restarts.");
        }
        return 0;

    case WM_SHOW_SETTINGS:  // second app instance launched
        settingsdialog::Show(_instance, _controller);
        return 0;

    case WM_TIMER:
        if (wParam == 1)
            UpdateTooltip();  // periodic fps refresh while streaming
        else if (wParam == 2)
        {
            KillTimer(hwnd, 2);   // one-shot: fires once per device change
            micwatch::CheckPs3Async(hwnd, WM_MIC_HEALTH);
        }
        return 0;

    case WM_MIC_HEALTH:
        // Only Wedged is worth interrupting anyone over. Unknown means the probe
        // could not tell (no camera, endpoint busy) and must never be dressed up
        // as a fault; Alive is the expected case and says nothing.
        if (static_cast<micwatch::Health>(wParam) == micwatch::Health::Wedged)
        {
            ShowBalloon(L"PS3 Eye microphone — please replug",
                        L"The camera's microphone has stopped delivering audio. "
                        L"Unplug the PS3 Eye and plug it back in — nothing else "
                        L"recovers it, and the video is unaffected.");
        }
        settingsdialog::RefreshStatus();
        return 0;

    case WM_CREATE:
        // No timer here: it is armed on the first Streaming transition below.
        return 0;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        // If the tray window dies for any reason the app must exit with it
        // (redundant during normal shutdown, where the loop has already left).
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
