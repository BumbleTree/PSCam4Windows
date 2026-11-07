#pragma once
//
// Native system primitives for the installer. Services, processes, COM
// registration, certificates, the registry and scheduled tasks are all driven
// through their APIs, so each step gets a concrete error code. The only child
// processes ever spawned are
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
    std::vector<CertRecord> ownedCertificates;   // OwnedCertificate_<thumbprint> values
    // Set by every install that records certificate ownership. An install
    // without it predates ownership tracking and removed its certificates by
    // payload thumbprint on uninstall.
    bool certOwnershipTracked = false;
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
// Adds to LocalMachine Root + TrustedPublisher, recording per-store whether we
// actually added it (pre-existing entries are left alone and not removed later).
bool AddCertToSystemStores(const BYTE* der, DWORD len, CertRecord& rec, Logger& log);
bool RemoveCertByThumbprint(const BYTE thumb[20], bool fromRoot, bool fromTrustedPub, Logger& log);
// Which LocalMachine stores hold the certificate: bit 0 Root, bit 1 TrustedPublisher.
DWORD CertStorePresence(const BYTE thumb[20]);
// ARP value name recording ownership of one certificate:
// "OwnedCertificate_<lowercase hex thumbprint>", REG_DWORD of CertStorePresence bits.
std::wstring CertificateOwnershipName(const BYTE thumb[20]);

// ---- drivers -----------------------------------------------------------------
// Runs %SystemRoot%\System32\pnputil.exe <args>, captures combined output,
// returns the raw exit code (0 / 259 staged / 3010 reboot / other = error).
DWORD RunPnputil(const std::wstring& args, std::wstring& output, Logger& log,
                 std::atomic<bool>* cancel = nullptr);
// Runs `exe` inside a kill-on-close job with stdout/stderr captured (UTF-16
// with BOM, else OEM code page). Returns the process exit code, or the Win32
// error that stopped it (ERROR_CANCELLED, ERROR_TIMEOUT, launch failure).
// A cancelled or timed-out child is terminated and waited for before return.
DWORD RunProcessCaptured(const std::wstring& exe, const std::wstring& args,
                         std::wstring& output, Logger& log,
                         std::atomic<bool>* cancel, DWORD timeoutMs);

// Is published package `oemName` (a bare "oemNN.inf" leaf) one of OUR driver
// packages: its driver-store INF leaf name equals originalLeafName AND it
// declares `interfaceGuid` -- and, with `exactInf`, its bytes equal that file.
//
// The GUID is what makes it ours. A Zadig/libwdi package for the same camera
// carries the same original INF name and a DIFFERENT, randomly generated
// interface GUID; matching on the name alone would let a rollback retract, or
// an uninstall delete, a package this installer never staged. Provider name
// cannot be used for this: our own pre-rebrand packages were themselves
// stamped "libwdi".
bool PublishedInfMatches(const std::wstring& oemName, const wchar_t* originalLeafName,
                         const GUID& interfaceGuid, const std::wstring* exactInf = nullptr);
// Every %WINDIR%\INF\oem*.inf for which PublishedInfMatches holds.
// Locale-independent (no pnputil text parsing). False when the scan fails.
bool EnumeratePublishedInfNames(const wchar_t* originalLeafName, const GUID& interfaceGuid,
                                std::vector<std::wstring>& names);

// The INF's [Strings] DeviceID equals `deviceId` exactly (case-insensitive).
// Distinguishes a whole-device package ("VID_1415&PID_2000") from an
// interface one ("VID_1415&PID_2000&MI_00") without parsing [Models].
bool InfDeviceIdIs(const std::wstring& infPath, const wchar_t* deviceId);

// Published packages of the PS3 Eye AUDIO driver an early release shipped
// (original name usb_audio.inf, [Version] Provider "PS3EyeVCam", bound to
// MI_01). It has no interface GUID, so the provider string -- our old product
// name -- is what identifies it as ours.
bool EnumerateLegacyPs3AudioInfNames(std::vector<std::wstring>& names);
bool PublishedInfIsLegacyPs3Audio(const std::wstring& oemName);

// A unique oemNNN.inf candidate from localized pnputil output. Ambiguous
// names are refused. Callers must verify package identity and exact INF bytes
// before recording it; output text alone does not establish ownership.
bool PublishedNameFromPnputilOutput(const std::wstring& output, std::wstring& oemName);

// ---- scheduled tasks -----------------------------------------------------------
bool ScheduledTaskExists(const wchar_t* name);
bool DeleteScheduledTask(const wchar_t* name, Logger& log);   // missing task = success

// ---- registry -------------------------------------------------------------------
bool RegKeyExists(HKEY root, const wchar_t* path);
LSTATUS CopyRegTree(HKEY root, const wchar_t* srcPath, const wchar_t* dstPath);

struct RegistryTree
{
    struct Key
    {
        struct Value { std::wstring name; DWORD type = 0; std::vector<BYTE> data; };
        std::wstring path;
        std::vector<Value> values;
    };
    bool existed = false;
    std::vector<Key> keys;
};
LSTATUS SnapshotRegistryTree(HKEY root, const wchar_t* path, RegistryTree& snapshot);
LSTATUS RestoreRegistryTree(HKEY root, const wchar_t* path, const RegistryTree& snapshot);

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
