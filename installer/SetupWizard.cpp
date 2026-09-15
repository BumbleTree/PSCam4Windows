#include "SetupWizard.h"

#include <objbase.h>
#include <commctrl.h>
#include <process.h>

#include "Payload.h"
#include "installer_resource.h"
#include "..\res\version.h"

namespace
{

struct PageText
{
    const wchar_t* title;
    const wchar_t* subtitle;
};

// Indexed by SetupWizard::Page (Finish is composed dynamically).
const PageText kPageText[] = {
    { L"Welcome to PSCam4Win Setup",
      L"Virtual cameras for the PS3 Eye, PS2 EyeToy, and PS4 Camera." },
    { L"License agreement",
      L"PSCam4Win is free software under the GNU GPL v2." },
    { L"Choose components",
      L"Select the camera drivers and options to install." },
    // Progress serves both engines; Navigate() swaps in the uninstall wording.
    { L"Installing",
      L"Setup is making the changes listed below." },
    { L"Setup complete", L"" },
    { L"Maintenance",
      L"PSCam4Win is already installed on this computer." },
    { L"Uninstall PSCam4Win",
      L"This removes the app, the WinUSB drivers, and the signing certificates." },
};

const WORD kPageDialogIds[] = {
    IDD_PAGE_WELCOME, IDD_PAGE_LICENSE, IDD_PAGE_COMPONENTS, IDD_PAGE_PROGRESS,
    IDD_PAGE_FINISH, IDD_PAGE_MAINTENANCE, IDD_PAGE_UNCONFIRM,
};

// The three frame buttons are the only chrome that varies from page to page.
struct ButtonState
{
    bool           back;
    const wchar_t* nextText;
    bool           next;
    bool           cancel;
};

// Process exit code: 0 = success, 1 = failed, 2 = cancelled / closed early.
int ExitCodeFor(EngineResult result)
{
    switch (result)
    {
    case EngineResult::Success:
    case EngineResult::SuccessReboot:
        return 0;
    case EngineResult::Cancelled:
        return 2;
    default:
        return 1;
    }
}

// Parses a "major.minor.patch" DisplayVersion string (the only shape this
// installer ever writes). Returns false if it doesn't look like a version.
bool ParseVersion(const std::wstring& text, int out[3])
{
    out[0] = out[1] = out[2] = 0;
    return swscanf_s(text.c_str(), L"%d.%d.%d", &out[0], &out[1], &out[2]) >= 1;
}

// <0 if a<b, 0 if equal, >0 if a>b.
int CompareVersion(const int a[3], const int b[3])
{
    for (int i = 0; i < 3; ++i)
        if (a[i] != b[i])
            return a[i] < b[i] ? -1 : 1;
    return 0;
}

} // namespace

int SetupWizard::Run(const Params& params)
{
    _params = params;

    _frame = CreateDialogParamW(params.instance, MAKEINTRESOURCEW(IDD_SETUP_FRAME),
                                nullptr, FrameProc, reinterpret_cast<LPARAM>(this));
    if (!_frame)
        return 1;
    ShowWindow(_frame, SW_SHOW);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0)
    {
        if (!IsDialogMessageW(_frame, &msg))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (_worker)
    {
        WaitForSingleObject(_worker, 30000);   // never orphan a running engine
        CloseHandle(_worker);
        _worker = nullptr;
    }
    return _exitCode;
}

INT_PTR CALLBACK SetupWizard::FrameProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SetupWizard* self;
    if (msg == WM_INITDIALOG)
    {
        self = reinterpret_cast<SetupWizard*>(lParam);
        SetWindowLongPtrW(wnd, DWLP_USER, lParam);
        self->_frame = wnd;
        self->OnInitFrame(wnd);
        return TRUE;
    }
    self = reinterpret_cast<SetupWizard*>(GetWindowLongPtrW(wnd, DWLP_USER));
    if (!self)
        return FALSE;
    return self->OnFrameMessage(wnd, msg, wParam, lParam);
}

INT_PTR CALLBACK SetupWizard::PageProc(HWND wnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INITDIALOG)
    {
        SetWindowLongPtrW(wnd, DWLP_USER, lParam);
        return TRUE;
    }
    if (msg == WM_COMMAND)
    {
        auto* self = reinterpret_cast<SetupWizard*>(GetWindowLongPtrW(wnd, DWLP_USER));
        if (self)
            self->OnPageCommand(wnd, LOWORD(wParam), HIWORD(wParam));
    }
    return FALSE;
}

INT_PTR SetupWizard::OnFrameMessage(HWND /*wnd*/, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case IDC_NEXT:   OnNext();   return TRUE;
        case IDC_BACK:   OnBack();   return TRUE;
        case IDCANCEL:   OnCancel(); return TRUE;
        }
        return FALSE;

    case WM_APP_LOG:
        OnEngineLog(reinterpret_cast<std::wstring*>(lParam));
        return TRUE;
    case WM_APP_PROGRESS:
        OnEngineProgress(static_cast<int>(wParam), static_cast<int>(lParam));
        return TRUE;
    case WM_APP_DONE:
        OnEngineDone(static_cast<EngineResult>(wParam));
        return TRUE;

    case WM_CLOSE:
        OnCancel();
        return TRUE;
    case WM_DESTROY:
        if (_boldFont)
        {
            DeleteObject(_boldFont);
            _boldFont = nullptr;
        }
        PostQuitMessage(_exitCode);
        return TRUE;
    }
    return FALSE;
}

void SetupWizard::OnInitFrame(HWND frame)
{
    const HICON icon = LoadIconW(_params.instance, MAKEINTRESOURCEW(IDI_SETUP));
    SendMessageW(frame, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(icon));
    SendMessageW(frame, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(icon));

    // Bold variant of the dialog font for the header + the reboot notice.
    const HFONT dialogFont =
        reinterpret_cast<HFONT>(SendMessageW(frame, WM_GETFONT, 0, 0));
    LOGFONTW lf = {};
    GetObjectW(dialogFont, sizeof(lf), &lf);
    lf.lfWeight = FW_BOLD;
    _boldFont = CreateFontIndirectW(&lf);
    SendDlgItemMessageW(frame, IDC_HEADER_TITLE, WM_SETFONT,
                        reinterpret_cast<WPARAM>(_boldFont), TRUE);

    for (int i = 0; i < PageCount; ++i)
        _pages[i] = CreateDialogParamW(_params.instance,
                                       MAKEINTRESOURCEW(kPageDialogIds[i]), frame,
                                       PageProc, reinterpret_cast<LPARAM>(this));

    // ---- static page content ------------------------------------------------
    wchar_t text[1024];
    _snwprintf_s(text, _TRUNCATE,
        L"This wizard installs PSCam4Win %s, the open-source virtual camera "
        L"suite that turns PlayStation cameras into standard Windows cameras "
        L"for Discord, OBS, browsers, RPCS3, and everything else.\r\n\r\n"
        L"Setup will install the camera drivers you select, register the "
        L"virtual camera with Windows, and add PSCam4Win to Apps & Features.\r\n\r\n"
        L"The camera drivers are Microsoft's own inbox WinUSB, but their INF "
        L"catalogs are self-signed. Windows will not accept a self-signed driver "
        L"package unless it trusts the signer, so setup adds each selected "
        L"package's certificate to the Trusted Root and Trusted Publishers "
        L"stores. Uninstalling removes them again.\r\n\r\n"
        L"Windows 11 is required. Administrator rights are required.",
        L"" PSCAM_VERSION_DISPLAY);
    SetDlgItemTextW(_pages[PageWelcome], IDC_WELCOME_TEXT, text);

    SetDlgItemTextW(_pages[PageLicense], IDC_LICENSE_EDIT,
                    payload::LoadLicenseText().c_str());

    CheckDlgButton(_pages[PageComponents], IDC_COMP_PS3, BST_CHECKED);
    CheckDlgButton(_pages[PageComponents], IDC_COMP_EYETOY, BST_CHECKED);
    CheckDlgButton(_pages[PageComponents], IDC_COMP_PS4, BST_CHECKED);
    CheckDlgButton(_pages[PageComponents], IDC_OPT_AUTOSTART, BST_CHECKED);
    CheckDlgButton(_pages[PageComponents], IDC_OPT_LAUNCH, BST_CHECKED);
    _snwprintf_s(text, _TRUNCATE, L"Installs to:  %s", ops::InstallDir().c_str());
    SetDlgItemTextW(_pages[PageComponents], IDC_COMP_PATH, text);
    _snwprintf_s(text, _TRUNCATE,
        L"PS4 Camera: setup downloads its firmware automatically "
        L"(integrity-checked) into %s; to install offline, put firmware.bin "
        L"beside this exe. Its microphone also needs a virtual audio cable "
        L"(VB-CABLE or similar) to work in other apps -- Settings links to one, "
        L"and PSCam4Win installs no audio driver itself.\r\n"
        L"PS3 Eye and EyeToy need nothing extra.",
        ops::ProgramDataDir().c_str());
    SetDlgItemTextW(_pages[PageComponents], IDC_COMP_NOTE, text);

    const std::wstring detectedVersion = _params.detected.displayVersion.empty()
        ? L"(unknown version)" : _params.detected.displayVersion;
    int installedVer[3] = {};
    const int currentVer[3] = { PSCAM_VERSION_MAJOR, PSCAM_VERSION_MINOR, PSCAM_VERSION_PATCH };
    const bool isUpdate = ParseVersion(_params.detected.displayVersion, installedVer) &&
                          CompareVersion(installedVer, currentVer) < 0;
    if (isUpdate)
    {
        _snwprintf_s(text, _TRUNCATE,
            L"PSCam4Win %s is installed. A newer version (%s) is available.\r\n\r\n"
            L"What would you like to do?",
            detectedVersion.c_str(), L"" PSCAM_VERSION_DISPLAY);
        SetDlgItemTextW(_pages[PageMaintenance], IDC_MAINT_TEXT, text);
        _snwprintf_s(text, _TRUNCATE, L"&Update to %s (keeps saved settings)",
                     L"" PSCAM_VERSION_DISPLAY);
        SetDlgItemTextW(_pages[PageMaintenance], IDC_MAINT_REINSTALL, text);
    }
    else
    {
        _snwprintf_s(text, _TRUNCATE,
            L"PSCam4Win %s is already installed on this computer.\r\n"
            L"This setup program is version %s.\r\n\r\nWhat would you like to do?",
            detectedVersion.c_str(), L"" PSCAM_VERSION_DISPLAY);
        SetDlgItemTextW(_pages[PageMaintenance], IDC_MAINT_TEXT, text);
    }
    CheckRadioButton(_pages[PageMaintenance], IDC_MAINT_REINSTALL,
                     IDC_MAINT_UNINSTALL, IDC_MAINT_REINSTALL);

    SetDlgItemTextW(_pages[PageUnConfirm], IDC_UN_TEXT,
        L"PSCam4Win Virtual Camera will be removed from this computer: the "
        L"tray app, the virtual camera registration, the WinUSB drivers for "
        L"all three cameras, and the driver-signing certificates.\r\n\r\n"
        L"Saved camera settings and the PS4 firmware cache are kept unless "
        L"you tick the box below.");

    switch (_params.mode)
    {
    case WizardMode::Install:     Navigate(PageWelcome);     break;
    case WizardMode::Maintenance: Navigate(PageMaintenance); break;
    case WizardMode::Uninstall:   Navigate(PageUnConfirm);   break;
    }
    if (_params.mode == WizardMode::Maintenance && isUpdate)
        SetDlgItemTextW(_frame, IDC_HEADER_SUB, L"An update is available for PSCam4Win.");
}

void SetupWizard::OnPageCommand(HWND page, WORD id, WORD code)
{
    if (page == _pages[PageLicense] && id == IDC_LICENSE_ACCEPT && code == BN_CLICKED &&
        _current == PageLicense)
    {
        EnableWindow(GetDlgItem(_frame, IDC_NEXT),
                     IsDlgButtonChecked(page, IDC_LICENSE_ACCEPT) == BST_CHECKED);
    }
}

void SetupWizard::OnNext()
{
    switch (_current)
    {
    case PageWelcome:
        Navigate(PageLicense);
        break;
    case PageLicense:
        Navigate(PageComponents);
        break;
    case PageComponents:
        StartEngine(false);
        Navigate(PageProgress);
        break;
    case PageMaintenance:
        if (IsDlgButtonChecked(_pages[PageMaintenance], IDC_MAINT_UNINSTALL) == BST_CHECKED)
            Navigate(PageUnConfirm);
        else
            Navigate(PageLicense);
        break;
    case PageUnConfirm:
        StartEngine(true);
        Navigate(PageProgress);
        break;
    case PageFinish:
        DestroyWindow(_frame);
        break;
    default:
        break;
    }
}

void SetupWizard::OnBack()
{
    switch (_current)
    {
    case PageLicense:
        Navigate(_params.mode == WizardMode::Maintenance ? PageMaintenance : PageWelcome);
        break;
    case PageComponents:
        Navigate(PageLicense);
        break;
    case PageUnConfirm:
        if (_params.mode == WizardMode::Maintenance)
            Navigate(PageMaintenance);
        break;
    default:
        break;
    }
}

void SetupWizard::OnCancel()
{
    if (_engineRunning)
    {
        if (_cancelFlag.load())
            return;   // already cancelling
        const wchar_t* prompt = _engineWasUninstall
            ? L"Stop removing PSCam4Win?\n\nItems already removed stay removed; "
              L"run the uninstaller again later to finish."
            : L"Cancel the installation?\n\nEverything done so far will be rolled back.";
        if (MessageBoxW(_frame, prompt, L"PSCam4Win Setup",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
            return;
        _cancelFlag.store(true);
        const HWND cancelBtn = GetDlgItem(_frame, IDCANCEL);
        SetWindowTextW(cancelBtn, L"Cancelling...");
        EnableWindow(cancelBtn, FALSE);
        return;
    }
    if (_finished)
        _exitCode = ExitCodeFor(_result);
    DestroyWindow(_frame);
}

void SetupWizard::Navigate(Page page)
{
    ShowWindow(_pages[_current], SW_HIDE);
    _current = page;

    SetDlgItemTextW(_frame, IDC_HEADER_TITLE, kPageText[page].title);
    SetDlgItemTextW(_frame, IDC_HEADER_SUB, kPageText[page].subtitle);
    if (page == PageProgress && _engineWasUninstall)
    {
        SetDlgItemTextW(_frame, IDC_HEADER_TITLE, L"Removing PSCam4Win");
        SetDlgItemTextW(_frame, IDC_HEADER_SUB,
                        L"Setup is removing PSCam4Win from this computer.");
    }

    const bool licenseAccepted =
        IsDlgButtonChecked(_pages[PageLicense], IDC_LICENSE_ACCEPT) == BST_CHECKED;
    const bool cameFromMaintenance = _params.mode == WizardMode::Maintenance;
    const bool uninstallOnly = _params.mode == WizardMode::Uninstall;

    ButtonState state = { false, L"&Next >", true, true };
    switch (page)
    {
    case PageWelcome:     state = { false,               L"&Next >",    true,            true  }; break;
    case PageLicense:     state = { !uninstallOnly,      L"&Next >",    licenseAccepted, true  }; break;
    case PageComponents:  state = { true,                L"&Install",   true,            true  }; break;
    case PageProgress:    state = { false,               L"&Next >",    false,           true  }; break;
    case PageFinish:      state = { false,               L"&Finish",    true,            false }; break;
    case PageMaintenance: state = { false,               L"&Next >",    true,            true  }; break;
    case PageUnConfirm:   state = { cameFromMaintenance, L"&Uninstall", true,            true  }; break;
    default:              break;
    }

    const HWND back = GetDlgItem(_frame, IDC_BACK);
    const HWND next = GetDlgItem(_frame, IDC_NEXT);
    const HWND cancel = GetDlgItem(_frame, IDCANCEL);
    EnableWindow(back, state.back);
    SetWindowTextW(next, state.nextText);
    EnableWindow(next, state.next);
    SetWindowTextW(cancel, L"Cancel");
    EnableWindow(cancel, state.cancel);

    ShowWindow(_pages[page], SW_SHOW);
    SendMessageW(_frame, WM_NEXTDLGCTL,
                 reinterpret_cast<WPARAM>(next), TRUE);
}

InstallOptions SetupWizard::CollectInstallOptions() const
{
    InstallOptions opts;
    const HWND page = _pages[PageComponents];
    opts.compPs3 = IsDlgButtonChecked(page, IDC_COMP_PS3) == BST_CHECKED;
    opts.compEyeToy = IsDlgButtonChecked(page, IDC_COMP_EYETOY) == BST_CHECKED;
    opts.compPs4 = IsDlgButtonChecked(page, IDC_COMP_PS4) == BST_CHECKED;
    opts.autostart = IsDlgButtonChecked(page, IDC_OPT_AUTOSTART) == BST_CHECKED;
    opts.launchAfter = IsDlgButtonChecked(page, IDC_OPT_LAUNCH) == BST_CHECKED;
    return opts;
}

void SetupWizard::StartEngine(bool uninstall)
{
    _ctx = std::make_unique<SetupContext>();
    _ctx->opts = uninstall ? InstallOptions{} : CollectInstallOptions();
    if (uninstall)
        _ctx->opts.removeData = IsDlgButtonChecked(_pages[PageUnConfirm],
                                                   IDC_UN_REMOVEDATA) == BST_CHECKED;
    _ctx->installDir = ops::InstallDir();
    _ctx->setupExePath = _params.setupExePath;
    _ctx->sourceDir = _params.sourceDir;
    _ctx->arpState = _params.detected;
    _ctx->log = &_logger;
    _ctx->cancel = &_cancelFlag;
    _ctx->notifyWnd = _frame;

    std::wstring logPath;
    if (uninstall)
    {
        wchar_t temp[MAX_PATH] = {};
        GetTempPathW(_countof(temp), temp);
        logPath = std::wstring(temp) + L"PSCam4Win-uninstall.log";
    }
    else
    {
        ops::EnsureDir(ops::ProgramDataDir());
        logPath = ops::JoinPath(ops::ProgramDataDir(), L"install.log");
    }
    _logger.Open(logPath);
    _logger.SetMirror(_frame, WM_APP_LOG);
    _logger.Line(L"PSCam4Win Setup %s -- %s", L"" PSCAM_VERSION_DISPLAY,
                 uninstall ? L"uninstall" : L"install");

    _cancelFlag.store(false);
    _engineWasUninstall = uninstall;
    _engineRunning = true;
    _worker = reinterpret_cast<HANDLE>(
        _beginthreadex(nullptr, 0, WorkerThunk, this, 0, nullptr));
    if (!_worker)
    {
        _engineRunning = false;
        _logger.Line(L"error: could not start the worker thread");
        OnEngineDone(EngineResult::Failed);
    }
}

unsigned __stdcall SetupWizard::WorkerThunk(void* param)
{
    static_cast<SetupWizard*>(param)->WorkerMain();
    return 0;
}

void SetupWizard::WorkerMain()
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    EngineResult result;
    {
        auto steps = _engineWasUninstall ? engine::BuildUninstallSteps()
                                         : engine::BuildInstallSteps();
        result = _engineWasUninstall ? engine::RunLinear(steps, *_ctx)
                                     : engine::Run(steps, *_ctx);
    }
    CoUninitialize();
    PostMessageW(_frame, WM_APP_DONE, static_cast<WPARAM>(result), 0);
}

void SetupWizard::OnEngineLog(std::wstring* line)
{
    if (!line)
        return;
    AppendLog(*line);
    if (line->compare(0, 4, L"=== ") == 0)
        SetDlgItemTextW(_pages[PageProgress], IDC_PROGRESS_STEP, line->c_str() + 4);
    delete line;
}

void SetupWizard::OnEngineProgress(int done, int total)
{
    const HWND bar = GetDlgItem(_pages[PageProgress], IDC_PROGRESS_BAR);
    SendMessageW(bar, PBM_SETRANGE32, 0, total);
    SendMessageW(bar, PBM_SETPOS, done, 0);
}

void SetupWizard::OnEngineDone(EngineResult result)
{
    _engineRunning = false;
    _finished = true;
    _result = result;
    if (_worker)
    {
        WaitForSingleObject(_worker, 5000);
        CloseHandle(_worker);
        _worker = nullptr;
    }

    _exitCode = ExitCodeFor(result);

    SetDlgItemTextW(_pages[PageFinish], IDC_FINISH_TEXT,
                    ComposeFinishText(result).c_str());
    const HWND reboot = GetDlgItem(_pages[PageFinish], IDC_FINISH_REBOOT);
    if (result == EngineResult::SuccessReboot)
    {
        SetWindowTextW(reboot, L"A restart is required before the camera drivers "
                               L"finish setting up.");
        SendMessageW(reboot, WM_SETFONT, reinterpret_cast<WPARAM>(_boldFont), TRUE);
        ShowWindow(reboot, SW_SHOW);
    }
    Navigate(PageFinish);

    const wchar_t* title;
    if (result == EngineResult::Success || result == EngineResult::SuccessReboot)
        title = _engineWasUninstall ? L"Uninstall complete" : L"Setup complete";
    else if (result == EngineResult::Cancelled)
        title = _engineWasUninstall ? L"Uninstall interrupted" : L"Setup cancelled";
    else
        title = _engineWasUninstall ? L"Uninstall finished with problems" : L"Setup failed";
    SetDlgItemTextW(_frame, IDC_HEADER_TITLE, title);
    SetDlgItemTextW(_frame, IDC_HEADER_SUB, L"");
}

std::wstring SetupWizard::ComposeFinishText(EngineResult result) const
{
    std::wstring text;
    if (_engineWasUninstall)
    {
        switch (result)
        {
        case EngineResult::Success:
        case EngineResult::SuccessReboot:
            text = L"PSCam4Win was removed from this computer.";
            if (_ctx && _ctx->warnings > 0)
            {
                wchar_t line[512];
                _snwprintf_s(line, _TRUNCATE,
                             L"\r\n\r\n%d item(s) could not be removed. Details:\r\n%s",
                             _ctx->warnings, _logger.Path().c_str());
                text += line;
            }
            if (_ctx && _ctx->opts.removeData)
                text += L"\r\n\r\nSaved settings and the firmware cache were removed too.";
            break;
        case EngineResult::Cancelled:
            text = L"The removal was interrupted. Run the uninstaller again to finish "
                   L"cleaning up.";
            break;
        default:
            text = L"The removal ran into problems. Details:\r\n" + _logger.Path();
            break;
        }
        return text;
    }

    switch (result)
    {
    case EngineResult::Success:
    case EngineResult::SuccessReboot:
    {
        text = L"PSCam4Win was installed successfully.";
        bool anyStaged = false;
        for (int i = 0; i < 3; ++i)
            anyStaged |= _ctx && _ctx->stagedDriver[i];
        if (anyStaged)
        {
            text += L"\r\n\r\nDrivers staged (they bind automatically on first plug-in):";
            for (int i = 0; i < 3; ++i)
                if (_ctx && _ctx->stagedDriver[i])
                {
                    text += L"\r\n    - ";
                    text += kDrivers[i].displayName;
                }
        }
        if (_ctx && _ctx->opts.compPs4)
        {
            const std::wstring fw = ops::JoinPath(ops::ProgramDataDir(), L"firmware.bin");
            if (!ops::PathExists(fw))
                text += L"\r\n\r\nPS4 Camera: firmware.bin could not be downloaded "
                        L"(offline?) -- re-run setup online or see the README "
                        L"before first use.";
            // Repeated from the components page because this is where people
            // look for what to do next, and the microphone is the one PS4
            // feature that needs something setup is not allowed to install
            // for them (every virtual cable is a third-party kernel driver).
            text += L"\r\n\r\nPS4 Camera microphone: it works inside "
                    L"PSCam4Win straight away. To use it in Discord, OBS or any "
                    L"other app, install a virtual audio cable (VB-CABLE or "
                    L"similar), then point Settings > Send to: at it. Settings "
                    L"links to one if none is installed.";
        }
        if (_ctx && _ctx->opts.launchAfter)
            text += L"\r\n\r\nThe camera host is running -- look for the camera icon "
                    L"in the system tray.";
        break;
    }
    case EngineResult::Cancelled:
        text = L"The installation was cancelled.\r\n\r\nEverything done so far was "
               L"rolled back; nothing was left on the system.";
        break;
    default:
        text = L"The installation failed and every completed step was rolled "
               L"back.\r\n\r\nDetails:\r\n" + _logger.Path();
        break;
    }
    return text;
}

void SetupWizard::AppendLog(const std::wstring& line)
{
    const HWND edit = GetDlgItem(_pages[PageProgress], IDC_PROGRESS_LOG);
    const int length = GetWindowTextLengthW(edit);
    SendMessageW(edit, EM_SETSEL, length, length);
    const std::wstring withBreak = line + L"\r\n";
    SendMessageW(edit, EM_REPLACESEL, FALSE,
                 reinterpret_cast<LPARAM>(withBreak.c_str()));
}
