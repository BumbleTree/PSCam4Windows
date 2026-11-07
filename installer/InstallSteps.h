#pragma once
//
// The install/uninstall step engine. Installation is a sequence of Step
// objects, each pairing a Run action with a best-effort Undo. On any failure
// (or user cancel between steps) the engine unwinds every completed step in
// reverse, including partial execution of the failed step. Recovery failures
// are reported and recoverable file backups are retained. Uninstall runs
// linearly with continue-on-error semantics (a stubborn driver should not stop
// the rest of the cleanup).
//
#include <windows.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "SetupOps.h"

class Logger;

// Wizard-selected options.
struct InstallOptions
{
    bool compPs3 = true;
    bool compEyeToy = true;
    bool compPs4 = true;
    bool autostart = true;
    bool launchAfter = true;
    bool removeData = false;   // uninstall: also delete settings + firmware cache
};

enum class EngineResult
{
    Success,
    SuccessReboot,   // finished, but pnputil reported 3010 somewhere
    Cancelled,
    Failed,
};

// Worker -> UI thread messages (lParam of WM_APP_LOG is a heap std::wstring*
// owned by the receiver).
constexpr UINT WM_APP_LOG      = WM_APP + 1;
constexpr UINT WM_APP_PROGRESS = WM_APP + 2;   // wParam = done steps, lParam = total
constexpr UINT WM_APP_DONE     = WM_APP + 3;   // wParam = EngineResult

// The three WinUSB driver packages.
struct DriverDef
{
    const wchar_t* displayName;   // "PS3 Eye"
    const wchar_t* infLeaf;       // "usb_device.inf"
    const wchar_t* arpOemValue;   // "PnpOem_usb_device"
    WORD           cerRes;        // embedded signing certificate
};
extern const DriverDef kDrivers[3];
bool DriverSelected(const InstallOptions& opts, int index);

// Everything the steps read and record. One instance lives for the whole
// engine run; Undo implementations consume the state their Run recorded.
struct SetupContext
{
    InstallOptions opts;
    std::wstring   installDir;      // ops::InstallDir()
    std::wstring   setupExePath;    // running module (may be the temp relaunch copy)
    std::wstring   sourceDir;       // dir of the ORIGINAL setup exe (firmware.bin lookup)
    InstallState   arpState;        // ARP state read at startup (certificate ownership)
    Logger*        log = nullptr;
    std::atomic<bool>* cancel = nullptr;
    HWND           notifyWnd = nullptr;

    // outcomes
    bool rebootRequired = false;
    bool stagedDriver[3] = {};      // pnputil 259: staged, binds on plug-in
    int  warnings = 0;              // uninstall: non-fatal problems
    bool rollbackIncomplete = false;

    // recorded state for undo
    std::wstring prevTrayPath;
    bool serviceWasRunning[2] = {};                       // Monitor, FrameServer
    bool dirCreated[3] = {};                              // install, driver, ProgramData
    std::vector<std::wstring> filesWritten;
    std::vector<std::pair<std::wstring, std::wstring>> backups;   // original -> backup copy
    std::wstring backupDir;
    std::vector<CertRecord> certsAdded;
    std::wstring oemInfName[3];
    std::vector<std::wstring> newDriverPackages[3];
    bool settingsKeyExisted = false;
    bool autostartTaskExisted = false;
};

class Step
{
public:
    virtual ~Step() = default;
    virtual const wchar_t* Name() const = 0;
    virtual bool Applies(const SetupContext&) const { return true; }
    virtual bool Run(SetupContext&) = 0;
    virtual void Undo(SetupContext&) noexcept {}
};

namespace engine
{

std::vector<std::unique_ptr<Step>> BuildInstallSteps();
std::vector<std::unique_ptr<Step>> BuildUninstallSteps();

// Install semantics: stop at the first failure and unwind attempted steps.
EngineResult Run(std::vector<std::unique_ptr<Step>>& steps, SetupContext& ctx);
// Uninstall semantics: run everything, count failures as ctx.warnings.
EngineResult RunLinear(std::vector<std::unique_ptr<Step>>& steps, SetupContext& ctx);

} // namespace engine
