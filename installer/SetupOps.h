#pragma once
//
// Native system primitives for the installer. Everything the old batch files
// did with net/taskkill/regsvr32/certutil/reg/schtasks is done through the
// real APIs here, so each step gets a concrete error code instead of a
// swallowed errorlevel. The only child processes ever spawned are
// %SystemRoot%\System32\pnputil.exe (driver store operations -- its staged /
// reboot-required exit codes are load-bearing) and the installed tray app's
// one-shot verbs (--seed-defaults / --enable-autostart).
//
#include <windows.h>

#include <atomic>
#include <string>
#include <vector>

class Logger;

// One driver certificate as installed into the system stores.
struct CertRecord
{
    BYTE thumbprint[20] = {};
    bool addedToRoot = false;        // false = was already present (not ours to remove)
    bool addedToTrustedPub = false;
};

// What the Add/Remove Programs key says about an existing install.
struct InstallState
{
    bool         installed = false;
    std::wstring displayVersion;
    std::wstring installLocation;
    std::wstring oemInf[3];          // persisted PnpOem_* values (may be empty)
};

namespace ops
{

// ---- environment -----------------------------------------------------------
bool IsWindows11OrLater();           // RtlGetVersion build >= 22000
bool IsProcessElevated();            // TokenElevation
std::wstring InstallDir();           // %ProgramFiles%\PSCam4Win
std::wstring LegacyInstallDir();     // %ProgramFiles%\PS3EyeVCam
std::wstring ProgramDataDir();       // %ProgramData%\PSCam4Win
bool ReadArpInstallState(InstallState& out);

// ---- services (FrameServer / FrameServerMonitor) ---------------------------
// Stops a service and everything that depends on it, waiting up to timeoutMs
// for SERVICE_STOPPED. A missing service counts as success with wasRunning
// = false.
bool StopServiceByName(const wchar_t* name, bool& wasRunning, DWORD timeoutMs, Logger& log);
bool StartServiceByName(const wchar_t* name, Logger& log);

// ---- processes --------------------------------------------------------------
// WM_CLOSE to the tray window class and wait for the process to exit.
// Returns true when no tray is left running (including "none was running").
bool CloseTrayGracefully(DWORD timeoutMs, Logger& log);
// TerminateProcess every process with this image name; returns count killed.
int  KillProcessByName(const wchar_t* exeName, Logger& log);

// ---- COM registration --------------------------------------------------------
// LoadLibrary + DllRegisterServer / DllUnregisterServer, real HRESULT out.
HRESULT RegisterComDll(const std::wstring& dllPath, bool unregister, Logger& log);
// Fallback cleanup: RegDeleteTree over the 8 static vcam CLSIDs (shared with
// the legacy PS3EyeVCam product).
void DeleteVCamClsids(Logger& log);

// ---- certificates ------------------------------------------------------------
bool ThumbprintOfCertBytes(const BYTE* der, DWORD len, BYTE out[20]);
bool ThumbprintOfCertFile(const std::wstring& path, BYTE out[20]);
// Adds to LocalMachine Root + TrustedPublisher, recording per-store whether we
// actually added it (pre-existing entries are left alone and not removed later).
bool AddCertToSystemStores(const BYTE* der, DWORD len, CertRecord& rec, Logger& log);
bool RemoveCertByThumbprint(const BYTE thumb[20], bool fromRoot, bool fromTrustedPub, Logger& log);

// ---- drivers -----------------------------------------------------------------
// Runs %SystemRoot%\System32\pnputil.exe <args>, captures combined output,
// returns the raw exit code (0 / 259 staged / 3010 reboot / other = error).
DWORD RunPnputil(const std::wstring& args, std::wstring& output, Logger& log);
// Locale-independent oemNN.inf lookup: scans C:\Windows\INF\oem*.inf and
// matches each package's driver-store INF leaf name against originalLeafName.
// Finds the published oem*.inf for one of OUR driver packages: the original
// leaf name matches AND the package declares `interfaceGuid`.
//
// The GUID is what makes this ours. A Zadig/libwdi package for the same camera
// carries the same original INF name and a DIFFERENT, randomly generated
// interface GUID -- and matching on the name alone picks whichever the INF
// directory lists first. That is how a rollback can retract, or an uninstall
// delete, a package this installer never staged. Provider name cannot be used
// for this: our own pre-rebrand packages were themselves stamped "libwdi".
bool FindPublishedInfName(const wchar_t* originalLeafName, const GUID& interfaceGuid,
                          std::wstring& oemName);

// The oemNNN.inf name pnputil says it published, pulled out of its output.
// This is the only EXACT answer to "which package did we just add": several of
// our own packages from earlier releases can share the original INF name and
// the interface GUID, so a driver-store scan cannot tell them apart.
//
// Matched by the value's shape rather than its label, because pnputil's output
// is localized and "Published Name" is not.
bool PublishedNameFromPnputilOutput(const std::wstring& output, std::wstring& oemName);

// ---- scheduled tasks -----------------------------------------------------------
bool ScheduledTaskExists(const wchar_t* name);
bool DeleteScheduledTask(const wchar_t* name, Logger& log);   // missing task = success

// ---- registry -------------------------------------------------------------------
bool RegKeyExists(HKEY root, const wchar_t* path);
LSTATUS CopyRegTree(HKEY root, const wchar_t* srcPath, const wchar_t* dstPath);

// ---- files / processes -------------------------------------------------------------
// Recursive delete restricted to an exact-match whitelist: the install dir,
// the legacy install dir, and %ProgramData%\PSCam4Win. Refuses to run if the
// current module lives inside the target. Retries once for slow handle
// releases.
bool DeleteDirectoryValidated(const std::wstring& dir, Logger& log);
// Recursive delete for the installer's own scratch dirs: the target must live
// under %TEMP% and its leaf name must start with "PSCam4Win-".
bool DeleteTempDirectory(const std::wstring& dir, Logger& log);
bool EnsureDir(const std::wstring& path);      // ok when it already exists
bool PathExists(const std::wstring& path);
std::wstring ModulePath();
std::wstring DirName(const std::wstring& path);
std::wstring JoinPath(const std::wstring& dir, const wchar_t* leaf);
bool RunAndWait(const std::wstring& exe, const std::wstring& args,
                DWORD& exitCode, DWORD timeoutMs, Logger& log);
bool LaunchDetached(const std::wstring& exe, const std::wstring& args);

// ---- network -----------------------------------------------------------------
// GET https://<host><path> into `out` (WinHTTP, TLS, HTTP 200 only, refuses
// bodies over maxBytes). `cancel` aborts between reads. Callers MUST verify
// the bytes (hash pin) before trusting them; this is transport only.
bool HttpsGet(const wchar_t* host, const wchar_t* path, std::vector<BYTE>& out,
              DWORD maxBytes, Logger& log, std::atomic<bool>* cancel);

} // namespace ops
