//
// PSCam4Win-Setup.exe -- GUI installer / uninstaller for PSCam4Win.
//
// Flags:
//   (none)                       install (or maintenance when already installed)
//   --uninstall                  remove PSCam4Win (Add/Remove Programs entry)
//   --relaunched <originalExe>   internal: this process is the %TEMP% copy that
//                                a setup exe launched from the install dir
//                                spawned so it can overwrite/delete that dir.
//
#include <windows.h>
#include <objbase.h>
#include <commctrl.h>
#include <shellapi.h>

#include <string>

#include "SetupOps.h"
#include "SetupWizard.h"

#pragma comment(lib, "comctl32.lib")

namespace
{

constexpr wchar_t kSetupMutexName[] = L"Global\\PSCam4Win.Setup.SingleInstance";

bool HasArg(int argc, wchar_t** argv, const wchar_t* name)
{
    for (int i = 1; i < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0)
            return true;
    return false;
}

std::wstring ArgValue(int argc, wchar_t** argv, const wchar_t* name)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (_wcsicmp(argv[i], name) == 0)
            return argv[i + 1];
    return std::wstring();
}

// Acquire the setup single-instance mutex. A --relaunched child races its
// exiting parent for a moment, so it retries briefly.
HANDLE AcquireSetupMutex(bool retry)
{
    const ULONGLONG deadline = GetTickCount64() + (retry ? 5000 : 0);
    for (;;)
    {
        HANDLE mutex = CreateMutexW(nullptr, TRUE, kSetupMutexName);
        if (mutex && GetLastError() != ERROR_ALREADY_EXISTS)
            return mutex;
        if (mutex)
            CloseHandle(mutex);
        if (GetTickCount64() >= deadline)
            return nullptr;
        Sleep(200);
    }
}

// Copy ourselves to %TEMP% and relaunch from there, so the copy in the
// install dir can be overwritten (reinstall) or deleted (uninstall).
bool RelaunchFromTemp(const std::wstring& selfPath, bool uninstall)
{
    wchar_t tempDir[MAX_PATH] = {};
    if (!GetTempPathW(_countof(tempDir), tempDir))
        return false;
    wchar_t leaf[64];
    swprintf_s(leaf, L"PSCam4Win-Setup-%lu.exe", GetCurrentProcessId());
    const std::wstring tempExe = ops::JoinPath(tempDir, leaf);

    if (!CopyFileW(selfPath.c_str(), tempExe.c_str(), FALSE))
        return false;

    std::wstring cmdLine = L"\"" + tempExe + L"\"";
    if (uninstall)
        cmdLine += L" --uninstall";
    cmdLine += L" --relaunched \"" + selfPath + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(tempExe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi))
    {
        DeleteFileW(tempExe.c_str());
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// A %TEMP% copy removes itself once the wizard is done: a detached cmd delete
// after a short delay, with a delete-on-reboot as the backstop.
void ScheduleSelfDelete(const std::wstring& selfPath)
{
    MoveFileExW(selfPath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);

    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, _countof(sysDir));
    const std::wstring cmd = std::wstring(sysDir) + L"\\cmd.exe";
    std::wstring cmdLine =
        L"\"" + cmd + L"\" /c ping -n 3 127.0.0.1 >nul & del \"" + selfPath + L"\"";

    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (CreateProcessW(cmd.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi))
    {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    const bool uninstall = HasArg(argc, argv, L"--uninstall");
    const std::wstring originalExe = ArgValue(argc, argv, L"--relaunched");
    const bool isTempCopy = !originalExe.empty();

    const std::wstring selfPath = ops::ModulePath();
    const std::wstring selfDir = ops::DirName(selfPath);

    // ---- gates ---------------------------------------------------------------
    if (!ops::IsWindows11OrLater())
    {
        MessageBoxW(nullptr,
            L"PSCam4Win requires Windows 11 (build 22000 or higher).\n\n"
            L"Windows 10 is not supported because it lacks the Media Foundation "
            L"Virtual Camera API (IMFVirtualCamera).",
            L"PSCam4Win Setup", MB_OK | MB_ICONERROR);
        return 1;
    }
    if (!ops::IsProcessElevated())
    {
        MessageBoxW(nullptr,
            L"Setup must run with administrator rights.\n\n"
            L"Right-click PSCam4Win-Setup.exe and choose \"Run as administrator\".",
            L"PSCam4Win Setup", MB_OK | MB_ICONERROR);
        return 1;
    }

    const HANDLE mutex = AcquireSetupMutex(isTempCopy);
    if (!mutex)
    {
        const HWND other = FindWindowW(nullptr, L"PSCam4Win Setup");
        if (other)
            SetForegroundWindow(other);
        else
            MessageBoxW(nullptr, L"PSCam4Win Setup is already running.",
                        L"PSCam4Win Setup", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    // ---- temp relaunch: never run the wizard from inside the install dir -----
    if (!isTempCopy && _wcsicmp(selfDir.c_str(), ops::InstallDir().c_str()) == 0)
    {
        CloseHandle(mutex);   // release before the child retries the acquire
        if (RelaunchFromTemp(selfPath, uninstall))
            return 0;
        MessageBoxW(nullptr,
            L"Setup could not restage itself outside the install directory.",
            L"PSCam4Win Setup", MB_OK | MB_ICONERROR);
        return 1;
    }

    // ---- mode + wizard ---------------------------------------------------------
    SetupWizard::Params params;
    params.instance = instance;
    params.setupExePath = selfPath;
    params.sourceDir = isTempCopy ? ops::DirName(originalExe) : selfDir;

    InstallState detected;
    ops::ReadArpInstallState(detected);
    params.detected = detected;
    if (uninstall)
        params.mode = WizardMode::Uninstall;
    else
        params.mode = detected.installed ? WizardMode::Maintenance : WizardMode::Install;

    INITCOMMONCONTROLSEX icc = { sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    int exitCode;
    {
        SetupWizard wizard;
        exitCode = wizard.Run(params);
    }

    CoUninitialize();
    CloseHandle(mutex);

    if (isTempCopy)
        ScheduleSelfDelete(selfPath);
    return exitCode;
}
