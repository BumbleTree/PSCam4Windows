#pragma once
//
// The wizard window: one frame dialog hosting seven child page dialogs
// (created once, toggled with ShowWindow -- same DIALOGEX/Segoe-UI idiom as
// host\SettingsDialog.cpp). The install/uninstall engine runs on a worker
// thread; the frame receives WM_APP_LOG / WM_APP_PROGRESS / WM_APP_DONE.
//
#include <windows.h>
#include <memory>
#include <string>

#include "InstallSteps.h"
#include "SetupLog.h"

enum class WizardMode { Install, Maintenance, Uninstall };

class SetupWizard
{
public:
    struct Params
    {
        HINSTANCE    instance = nullptr;
        WizardMode   mode = WizardMode::Install;
        InstallState detected;         // ARP state read at startup
        std::wstring setupExePath;     // running module (may be the temp copy)
        std::wstring sourceDir;        // dir of the ORIGINAL setup exe
    };

    // Blocks until the wizard closes. Returns the process exit code:
    // 0 = success, 1 = failed, 2 = cancelled / closed early.
    int Run(const Params& params);

private:
    enum Page
    {
        PageWelcome = 0, PageLicense, PageComponents, PageProgress, PageFinish,
        PageMaintenance, PageUnConfirm, PageCount
    };

    static INT_PTR CALLBACK FrameProc(HWND, UINT, WPARAM, LPARAM);
    static INT_PTR CALLBACK PageProc(HWND, UINT, WPARAM, LPARAM);
    static unsigned __stdcall WorkerThunk(void*);

    INT_PTR OnFrameMessage(HWND, UINT, WPARAM, LPARAM);
    void OnInitFrame(HWND frame);
    void OnPageCommand(HWND page, WORD id, WORD code);
    void OnNext();
    void OnBack();
    void OnCancel();
    void Navigate(Page page);
    void StartEngine(bool uninstall);
    void WorkerMain();
    void OnEngineLog(std::wstring* line);
    void OnEngineProgress(int done, int total);
    void OnEngineDone(EngineResult result);
    void AppendLog(const std::wstring& line);
    InstallOptions CollectInstallOptions() const;
    std::wstring ComposeFinishText(EngineResult result) const;

    Params        _params;
    HWND          _frame = nullptr;
    HWND          _pages[PageCount] = {};
    Page          _current = PageWelcome;
    HFONT         _boldFont = nullptr;
    HANDLE        _worker = nullptr;
    bool          _engineRunning = false;
    bool          _engineWasUninstall = false;
    bool          _finished = false;
    EngineResult  _result = EngineResult::Failed;
    int           _exitCode = 2;
    std::atomic<bool> _cancelFlag{ false };
    Logger        _logger;
    std::unique_ptr<SetupContext> _ctx;
};
