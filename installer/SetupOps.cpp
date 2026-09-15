#include "SetupOps.h"

#include <shlobj.h>
#include <knownfolders.h>
#include <tlhelp32.h>
#include <wincrypt.h>
#include <setupapi.h>
#include <taskschd.h>
#include <winhttp.h>
#include <wrl/client.h>

// winhttp.h hides this behind _WIN32_WINNT >= 0x0603 (Win 8.1), which the
// build does not define; the installer already requires Windows 11, where the
// flag is fully supported.
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY_CONFIG
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY_CONFIG 4
#endif

#include <vector>

#include "SetupLog.h"
#include "..\common\VCamGuids.h"

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "taskschd.lib")
#pragma comment(lib, "winhttp.lib")

using Microsoft::WRL::ComPtr;

namespace ops
{

namespace
{

// Tiny BSTR RAII (same idiom as host\Autostart.cpp -- no comdef.h needed).
class Bstr
{
public:
    explicit Bstr(const wchar_t* s) : _b(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(_b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    operator BSTR() const { return _b; }
private:
    BSTR _b;
};

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR raw = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &raw)))
        result = raw;
    CoTaskMemFree(raw);
    return result;
}

HRESULT ConnectTaskService(ComPtr<ITaskService>& service, ComPtr<ITaskFolder>& root)
{
    HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&service));
    if (FAILED(hr)) return hr;
    VARIANT empty;
    VariantInit(&empty);
    hr = service->Connect(empty, empty, empty, empty);
    if (FAILED(hr)) return hr;
    return service->GetFolder(Bstr(L"\\"), &root);
}

// Case-insensitive "is `inner` the same as or beneath `outer`".
bool PathIsInside(const std::wstring& inner, const std::wstring& outer)
{
    if (inner.size() < outer.size())
        return false;
    if (_wcsnicmp(inner.c_str(), outer.c_str(), outer.size()) != 0)
        return false;
    return inner.size() == outer.size() || inner[outer.size()] == L'\\';
}

std::wstring Canonicalize(const std::wstring& path)
{
    wchar_t full[MAX_PATH * 2] = {};
    const DWORD len = GetFullPathNameW(path.c_str(), _countof(full), full, nullptr);
    if (len == 0 || len >= _countof(full))
        return std::wstring();
    std::wstring result(full, len);
    while (!result.empty() && (result.back() == L'\\' || result.back() == L'/'))
        result.pop_back();
    return result;
}

bool StopOneService(SC_HANDLE scm, const wchar_t* name, DWORD timeoutMs,
                    bool* wasRunning, Logger& log);

// `net stop /y` equivalent: dependents first, then the service itself.
bool StopDependents(SC_HANDLE scm, SC_HANDLE service, DWORD timeoutMs, Logger& log)
{
    DWORD bytesNeeded = 0, count = 0;
    if (EnumDependentServicesW(service, SERVICE_ACTIVE, nullptr, 0, &bytesNeeded, &count))
        return true;                                   // no active dependents
    if (GetLastError() != ERROR_MORE_DATA)
        return false;

    std::vector<BYTE> buffer(bytesNeeded);
    auto* deps = reinterpret_cast<LPENUM_SERVICE_STATUSW>(buffer.data());
    if (!EnumDependentServicesW(service, SERVICE_ACTIVE, deps, bytesNeeded, &bytesNeeded, &count))
        return false;

    bool allStopped = true;
    for (DWORD i = 0; i < count; ++i)
        allStopped &= StopOneService(scm, deps[i].lpServiceName, timeoutMs, nullptr, log);
    return allStopped;
}

bool StopOneService(SC_HANDLE scm, const wchar_t* name, DWORD timeoutMs,
                    bool* wasRunning, Logger& log)
{
    if (wasRunning)
        *wasRunning = false;

    SC_HANDLE service = OpenServiceW(scm, name,
        SERVICE_STOP | SERVICE_QUERY_STATUS | SERVICE_ENUMERATE_DEPENDENTS);
    if (!service)
    {
        if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
            return true;                               // nothing to stop
        log.Line(L"warning: cannot open service %s (code %lu)", name, GetLastError());
        return false;
    }

    bool ok = true;
    SERVICE_STATUS_PROCESS status = {};
    DWORD bytes = 0;
    if (QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                             reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes))
    {
        if (status.dwCurrentState != SERVICE_STOPPED)
        {
            if (wasRunning)
                *wasRunning = true;

            StopDependents(scm, service, timeoutMs, log);

            SERVICE_STATUS ss = {};
            if (!ControlService(service, SERVICE_CONTROL_STOP, &ss) &&
                GetLastError() != ERROR_SERVICE_NOT_ACTIVE &&
                GetLastError() != ERROR_SERVICE_CANNOT_ACCEPT_CTRL)
            {
                log.Line(L"warning: stop request for %s failed (code %lu)", name, GetLastError());
            }

            const ULONGLONG deadline = GetTickCount64() + timeoutMs;
            for (;;)
            {
                if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                                          reinterpret_cast<BYTE*>(&status), sizeof(status), &bytes))
                    break;
                if (status.dwCurrentState == SERVICE_STOPPED)
                    break;
                if (GetTickCount64() >= deadline)
                {
                    log.Line(L"warning: service %s did not stop within %lu ms", name, timeoutMs);
                    ok = false;
                    break;
                }
                Sleep(250);
            }
        }
    }
    CloseServiceHandle(service);
    return ok;
}

} // namespace

// ---- environment -----------------------------------------------------------

bool IsWindows11OrLater()
{
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll)
        return false;
    const auto rtlGetVersion =
        reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion)
        return false;
    RTL_OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0)
        return false;
    return info.dwBuildNumber >= 22000;
}

bool IsProcessElevated()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation = {};
    DWORD size = 0;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation,
                                        sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

std::wstring InstallDir()
{
    return KnownFolder(FOLDERID_ProgramFiles) + L"\\PSCam4Win";
}

std::wstring LegacyInstallDir()
{
    return KnownFolder(FOLDERID_ProgramFiles) + L"\\PS3EyeVCam";
}

std::wstring ProgramDataDir()
{
    return KnownFolder(FOLDERID_ProgramData) + L"\\PSCam4Win";
}

bool ReadArpInstallState(InstallState& out)
{
    out = InstallState{};
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PSCam4Win",
                      0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;

    out.installed = true;
    auto readString = [key](const wchar_t* name) -> std::wstring {
        wchar_t buf[512] = {};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(key, name, nullptr, &type,
                             reinterpret_cast<BYTE*>(buf), &size) == ERROR_SUCCESS &&
            type == REG_SZ)
        {
            buf[_countof(buf) - 1] = L'\0';
            return buf;
        }
        return std::wstring();
    };
    out.displayVersion = readString(L"DisplayVersion");
    out.installLocation = readString(L"InstallLocation");
    out.oemInf[0] = readString(L"PnpOem_usb_device");
    out.oemInf[1] = readString(L"PnpOem_eyetoy_device");
    out.oemInf[2] = readString(L"PnpOem_ps4cam_device");
    RegCloseKey(key);
    return true;
}

// ---- services ---------------------------------------------------------------

bool StopServiceByName(const wchar_t* name, bool& wasRunning, DWORD timeoutMs, Logger& log)
{
    wasRunning = false;
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
    {
        log.Line(L"warning: cannot connect to the service manager (code %lu)", GetLastError());
        return false;
    }
    const bool ok = StopOneService(scm, name, timeoutMs, &wasRunning, log);
    CloseServiceHandle(scm);
    return ok;
}

bool StartServiceByName(const wchar_t* name, Logger& log)
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm)
        return false;
    bool ok = false;
    SC_HANDLE service = OpenServiceW(scm, name, SERVICE_START);
    if (service)
    {
        ok = StartServiceW(service, 0, nullptr) ||
             GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
        if (!ok)
            log.Line(L"warning: could not start service %s (code %lu)", name, GetLastError());
        CloseServiceHandle(service);
    }
    else if (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST)
    {
        ok = true;
    }
    CloseServiceHandle(scm);
    return ok;
}

// ---- processes ----------------------------------------------------------------

bool CloseTrayGracefully(DWORD timeoutMs, Logger& log)
{
    const HWND tray = FindWindowW(L"PSCam4WinTrayWnd", nullptr);
    if (!tray)
        return true;

    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    HANDLE process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;

    PostMessageW(tray, WM_CLOSE, 0, 0);

    bool exited = true;
    if (process)
    {
        exited = WaitForSingleObject(process, timeoutMs) == WAIT_OBJECT_0;
        CloseHandle(process);
    }
    else
    {
        Sleep(1000);
        exited = FindWindowW(L"PSCam4WinTrayWnd", nullptr) == nullptr;
    }
    if (exited)
        log.Line(L"tray app closed gracefully");
    return exited;
}

int KillProcessByName(const wchar_t* exeName, Logger& log)
{
    int killed = 0;
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;

    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    const DWORD ownPid = GetCurrentProcessId();
    if (Process32FirstW(snapshot, &entry))
    {
        do
        {
            if (entry.th32ProcessID == ownPid ||
                _wcsicmp(entry.szExeFile, exeName) != 0)
                continue;
            HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE,
                                         entry.th32ProcessID);
            if (!process)
                continue;
            if (TerminateProcess(process, 1))
            {
                WaitForSingleObject(process, 3000);
                ++killed;
                log.Line(L"terminated %s (pid %lu)", exeName, entry.th32ProcessID);
            }
            CloseHandle(process);
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return killed;
}

// ---- COM registration -----------------------------------------------------------

HRESULT RegisterComDll(const std::wstring& dllPath, bool unregister, Logger& log)
{
    const HMODULE dll = LoadLibraryExW(dllPath.c_str(), nullptr,
                                       LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!dll)
    {
        const HRESULT hr = HRESULT_FROM_WIN32(GetLastError());
        log.Line(L"error: cannot load %s (hr=0x%08X)", dllPath.c_str(), hr);
        return hr;
    }

    using RegFn = HRESULT(__stdcall*)();
    const char* entry = unregister ? "DllUnregisterServer" : "DllRegisterServer";
    const auto fn = reinterpret_cast<RegFn>(GetProcAddress(dll, entry));
    HRESULT hr;
    if (!fn)
    {
        hr = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        log.Line(L"error: %s not exported by %s", unregister ? L"DllUnregisterServer"
                                                             : L"DllRegisterServer",
                 dllPath.c_str());
    }
    else
    {
        hr = fn();
        if (FAILED(hr))
            log.Line(L"error: %s returned 0x%08X", unregister ? L"DllUnregisterServer"
                                                              : L"DllRegisterServer", hr);
    }
    FreeLibrary(dll);
    return hr;
}

void DeleteVCamClsids(Logger& log)
{
    for (int i = 0; i < kVCamCount; ++i)
    {
        wchar_t keyPath[128];
        swprintf_s(keyPath, L"Software\\Classes\\CLSID\\%s", kVCamClsidStrings[i]);
        const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, keyPath);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            log.Line(L"warning: could not delete %s (code %ld)", keyPath, status);
    }
}

// ---- certificates -----------------------------------------------------------------

bool ThumbprintOfCertBytes(const BYTE* der, DWORD len, BYTE out[20])
{
    PCCERT_CONTEXT context = CertCreateCertificateContext(X509_ASN_ENCODING, der, len);
    if (!context)
        return false;
    DWORD size = 20;
    const BOOL ok = CertGetCertificateContextProperty(context, CERT_SHA1_HASH_PROP_ID,
                                                      out, &size);
    CertFreeCertificateContext(context);
    return ok && size == 20;
}

bool ThumbprintOfCertFile(const std::wstring& path, BYTE out[20])
{
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const DWORD size = GetFileSize(file, nullptr);
    bool ok = false;
    if (size > 0 && size < 1024 * 1024)
    {
        std::vector<BYTE> bytes(size);
        DWORD read = 0;
        if (ReadFile(file, bytes.data(), size, &read, nullptr) && read == size)
            ok = ThumbprintOfCertBytes(bytes.data(), size, out);
    }
    CloseHandle(file);
    return ok;
}

namespace
{

HCERTSTORE OpenSystemStore(const wchar_t* name)
{
    return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                         CERT_SYSTEM_STORE_LOCAL_MACHINE, name);
}

#ifndef PSCAM_PUBLIC_RELEASE
bool StoreContainsThumbprint(HCERTSTORE store, const BYTE thumb[20])
{
    CRYPT_HASH_BLOB blob = { 20, const_cast<BYTE*>(thumb) };
    PCCERT_CONTEXT found = CertFindCertificateInStore(
        store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
    if (found)
    {
        CertFreeCertificateContext(found);
        return true;
    }
    return false;
}
#endif

} // namespace

bool AddCertToSystemStores(const BYTE* der, DWORD len, CertRecord& rec, Logger& log)
{
#ifdef PSCAM_PUBLIC_RELEASE
    // Public packages rely on Windows certificate policy, never installer-added trust.
    (void)der;
    (void)len;
    (void)rec;
    log.Line(L"Public release: certificate stores are left unchanged");
    return true;
#else
    if (!ThumbprintOfCertBytes(der, len, rec.thumbprint))
    {
        log.Line(L"error: embedded certificate is not a valid X.509 blob");
        return false;
    }

    const struct { const wchar_t* store; bool CertRecord::*added; } kStores[] = {
        { L"Root",             &CertRecord::addedToRoot },
        { L"TrustedPublisher", &CertRecord::addedToTrustedPub },
    };

    for (const auto& target : kStores)
    {
        const HCERTSTORE store = OpenSystemStore(target.store);
        if (!store)
        {
            log.Line(L"error: cannot open the LocalMachine %s store (code %lu)",
                     target.store, GetLastError());
            return false;
        }
        bool ok = true;
        if (StoreContainsThumbprint(store, rec.thumbprint))
        {
            log.Line(L"certificate already present in %s -- leaving it in place", target.store);
        }
        else if (CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, der, len,
                                                  CERT_STORE_ADD_REPLACE_EXISTING, nullptr))
        {
            rec.*(target.added) = true;
            log.Line(L"certificate added to LocalMachine\\%s", target.store);
        }
        else
        {
            log.Line(L"error: could not add the certificate to %s (code %lu)",
                     target.store, GetLastError());
            ok = false;
        }
        CertCloseStore(store, 0);
        if (!ok)
            return false;
    }
    return true;
#endif
}

bool RemoveCertByThumbprint(const BYTE thumb[20], bool fromRoot, bool fromTrustedPub, Logger& log)
{
    const struct { const wchar_t* store; bool enabled; } kStores[] = {
        { L"Root",             fromRoot },
        { L"TrustedPublisher", fromTrustedPub },
    };
    bool allOk = true;
    for (const auto& target : kStores)
    {
        if (!target.enabled)
            continue;
        const HCERTSTORE store = OpenSystemStore(target.store);
        if (!store)
        {
            allOk = false;
            continue;
        }
        CRYPT_HASH_BLOB blob = { 20, const_cast<BYTE*>(thumb) };
        PCCERT_CONTEXT found = CertFindCertificateInStore(
            store, X509_ASN_ENCODING, 0, CERT_FIND_SHA1_HASH, &blob, nullptr);
        if (found)
        {
            if (CertDeleteCertificateFromStore(found))   // consumes `found`
                log.Line(L"certificate removed from LocalMachine\\%s", target.store);
            else
            {
                log.Line(L"warning: could not remove the certificate from %s (code %lu)",
                         target.store, GetLastError());
                allOk = false;
            }
        }
        CertCloseStore(store, 0);
    }
    return allOk;
}

// ---- drivers --------------------------------------------------------------------

DWORD RunPnputil(const std::wstring& args, std::wstring& output, Logger& log)
{
    output.clear();

    wchar_t sysDir[MAX_PATH] = {};
    GetSystemDirectoryW(sysDir, _countof(sysDir));
    const std::wstring exe = std::wstring(sysDir) + L"\\pnputil.exe";

    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0))
        return static_cast<DWORD>(-1);
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdLine = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION pi = {};

    const BOOL launched = CreateProcessW(exe.c_str(), cmdLine.data(), nullptr, nullptr,
                                         TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(writePipe);
    if (!launched)
    {
        const DWORD code = GetLastError();
        CloseHandle(readPipe);
        log.Line(L"error: cannot launch pnputil.exe (code %lu)", code);
        return static_cast<DWORD>(-1);
    }

    std::vector<char> raw;
    char chunk[4096];
    DWORD read = 0;
    while (ReadFile(readPipe, chunk, sizeof(chunk), &read, nullptr) && read > 0)
        raw.insert(raw.end(), chunk, chunk + read);
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, 120000);
    DWORD exitCode = static_cast<DWORD>(-1);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // pnputil output is UTF-16LE when it starts with a BOM, OEM code page otherwise.
    if (raw.size() >= 2 && static_cast<BYTE>(raw[0]) == 0xFF && static_cast<BYTE>(raw[1]) == 0xFE)
    {
        output.assign(reinterpret_cast<const wchar_t*>(raw.data() + 2),
                      (raw.size() - 2) / sizeof(wchar_t));
    }
    else if (!raw.empty())
    {
        const int wideLen = MultiByteToWideChar(CP_OEMCP, 0, raw.data(),
                                                static_cast<int>(raw.size()), nullptr, 0);
        output.resize(static_cast<size_t>(wideLen));
        MultiByteToWideChar(CP_OEMCP, 0, raw.data(), static_cast<int>(raw.size()),
                            output.data(), wideLen);
    }
    while (!output.empty() && (output.back() == L'\r' || output.back() == L'\n' ||
                               output.back() == L' '))
        output.pop_back();
    return exitCode;
}

// Does this published INF declare the device-interface GUID we ship for the
// camera? Read through setupapi rather than by scanning the text, so INF quoting
// and the file's encoding are somebody else's problem.
static bool InfDeclaresInterfaceGuid(const std::wstring& infPath, const GUID& want)
{
    wchar_t wanted[64] = {};
    if (StringFromGUID2(want, wanted, _countof(wanted)) == 0)
        return false;

    const HINF inf = SetupOpenInfFileW(infPath.c_str(), nullptr, INF_STYLE_WIN4, nullptr);
    if (inf == INVALID_HANDLE_VALUE)
        return false;

    bool match = false;
    INFCONTEXT ctx = {};
    if (SetupFindFirstLineW(inf, L"Strings", L"DeviceGUID", &ctx))
    {
        wchar_t got[64] = {};
        if (SetupGetStringFieldW(&ctx, 1, got, _countof(got), nullptr))
            match = _wcsicmp(got, wanted) == 0;
    }
    SetupCloseInfFile(inf);
    return match;
}

bool PublishedNameFromPnputilOutput(const std::wstring& output, std::wstring& oemName)
{
    oemName.clear();
    for (size_t i = 0; i + 3 < output.size(); ++i)
    {
        if (_wcsnicmp(output.c_str() + i, L"oem", 3) != 0)
            continue;
        size_t j = i + 3;
        while (j < output.size() && output[j] >= L'0' && output[j] <= L'9')
            ++j;
        if (j == i + 3 || j + 4 > output.size())
            continue;                                   // "oem" with no number
        if (_wcsnicmp(output.c_str() + j, L".inf", 4) != 0)
            continue;
        oemName.assign(output, i, (j + 4) - i);
        return true;
    }
    return false;
}

bool FindPublishedInfName(const wchar_t* originalLeafName, const GUID& interfaceGuid,
                          std::wstring& oemName)
{
    oemName.clear();

    wchar_t winDir[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(winDir, _countof(winDir)))
        return false;
    const std::wstring infDir = std::wstring(winDir) + L"\\INF";

    WIN32_FIND_DATAW find = {};
    const HANDLE search = FindFirstFileW((infDir + L"\\oem*.inf").c_str(), &find);
    if (search == INVALID_HANDLE_VALUE)
        return false;

    bool found = false;
    do
    {
        const std::wstring oemPath = infDir + L"\\" + find.cFileName;
        wchar_t storePath[MAX_PATH * 2] = {};
        DWORD required = 0;
        if (!SetupGetInfDriverStoreLocationW(oemPath.c_str(), nullptr, nullptr,
                                             storePath, _countof(storePath), &required))
            continue;
        const wchar_t* leaf = wcsrchr(storePath, L'\\');
        leaf = leaf ? leaf + 1 : storePath;
        if (_wcsicmp(leaf, originalLeafName) == 0 &&
            InfDeclaresInterfaceGuid(oemPath, interfaceGuid))
        {
            oemName = find.cFileName;
            found = true;
            break;
        }
    } while (FindNextFileW(search, &find));
    FindClose(search);
    return found;
}

// ---- scheduled tasks ----------------------------------------------------------------

bool ScheduledTaskExists(const wchar_t* name)
{
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    if (FAILED(ConnectTaskService(service, root)))
        return false;
    ComPtr<IRegisteredTask> task;
    return SUCCEEDED(root->GetTask(Bstr(name), &task)) && task;
}

bool DeleteScheduledTask(const wchar_t* name, Logger& log)
{
    ComPtr<ITaskService> service;
    ComPtr<ITaskFolder> root;
    const HRESULT connect = ConnectTaskService(service, root);
    if (FAILED(connect))
    {
        log.Line(L"warning: cannot connect to the Task Scheduler (hr=0x%08X)", connect);
        return false;
    }
    const HRESULT hr = root->DeleteTask(Bstr(name), 0);
    if (SUCCEEDED(hr) || hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))
        return true;
    log.Line(L"warning: could not delete scheduled task %s (hr=0x%08X)", name, hr);
    return false;
}

// ---- registry -----------------------------------------------------------------------

bool RegKeyExists(HKEY root, const wchar_t* path)
{
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &key) != ERROR_SUCCESS)
        return false;
    RegCloseKey(key);
    return true;
}

LSTATUS CopyRegTree(HKEY root, const wchar_t* srcPath, const wchar_t* dstPath)
{
    HKEY src = nullptr;
    LSTATUS status = RegOpenKeyExW(root, srcPath, 0, KEY_READ, &src);
    if (status != ERROR_SUCCESS)
        return status;

    HKEY dst = nullptr;
    status = RegCreateKeyExW(root, dstPath, 0, nullptr, 0, KEY_WRITE, nullptr, &dst, nullptr);
    if (status == ERROR_SUCCESS)
    {
        status = RegCopyTreeW(src, nullptr, dst);
        RegCloseKey(dst);
    }
    RegCloseKey(src);
    return status;
}

// ---- files / processes -----------------------------------------------------------------

namespace
{

bool DeleteTreeUnchecked(const std::wstring& dir)
{
    WIN32_FIND_DATAW find = {};
    const HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &find);
    if (search != INVALID_HANDLE_VALUE)
    {
        do
        {
            if (wcscmp(find.cFileName, L".") == 0 || wcscmp(find.cFileName, L"..") == 0)
                continue;
            const std::wstring child = dir + L"\\" + find.cFileName;
            if (find.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                DeleteTreeUnchecked(child);
            }
            else
            {
                SetFileAttributesW(child.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child.c_str());
            }
        } while (FindNextFileW(search, &find));
        FindClose(search);
    }
    return RemoveDirectoryW(dir.c_str()) != 0;
}

} // namespace

bool DeleteDirectoryValidated(const std::wstring& dir, Logger& log)
{
    const std::wstring target = Canonicalize(dir);
    if (target.empty())
        return false;

    const std::wstring allowed[] = { InstallDir(), LegacyInstallDir(), ProgramDataDir() };
    bool whitelisted = false;
    for (const auto& candidate : allowed)
        whitelisted |= (_wcsicmp(target.c_str(), candidate.c_str()) == 0);
    if (!whitelisted)
    {
        log.Line(L"refusing to delete %s: not an expected install directory", target.c_str());
        return false;
    }
    if (PathIsInside(Canonicalize(ModulePath()), target))
    {
        log.Line(L"refusing to delete %s: this setup program is running from inside it",
                 target.c_str());
        return false;
    }
    if (!PathExists(target))
        return true;

    if (DeleteTreeUnchecked(target))
        return true;
    Sleep(600);                                  // straggling handle releases
    if (DeleteTreeUnchecked(target))
        return true;
    log.Line(L"warning: could not fully remove %s (code %lu)", target.c_str(), GetLastError());
    return false;
}

bool DeleteTempDirectory(const std::wstring& dir, Logger& log)
{
    const std::wstring target = Canonicalize(dir);
    if (target.empty() || !PathExists(target))
        return true;

    wchar_t tempRaw[MAX_PATH] = {};
    GetTempPathW(_countof(tempRaw), tempRaw);
    const std::wstring tempDir = Canonicalize(tempRaw);

    const size_t slash = target.find_last_of(L'\\');
    const std::wstring leaf = slash == std::wstring::npos
                                  ? target : target.substr(slash + 1);
    if (tempDir.empty() || !PathIsInside(target, tempDir) ||
        _wcsnicmp(leaf.c_str(), L"PSCam4Win-", 10) != 0)
    {
        log.Line(L"refusing to delete %s: not an installer scratch directory",
                 target.c_str());
        return false;
    }
    return DeleteTreeUnchecked(target);
}

bool EnsureDir(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr))
        return true;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

bool PathExists(const std::wstring& path)
{
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

std::wstring ModulePath()
{
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, path, _countof(path));
    return path;
}

std::wstring DirName(const std::wstring& path)
{
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring JoinPath(const std::wstring& dir, const wchar_t* leaf)
{
    std::wstring result = dir;
    if (!result.empty() && result.back() != L'\\')
        result += L'\\';
    result += leaf;
    return result;
}

bool RunAndWait(const std::wstring& exe, const std::wstring& args,
                DWORD& exitCode, DWORD timeoutMs, Logger& log)
{
    exitCode = static_cast<DWORD>(-1);
    std::wstring cmdLine = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
    {
        log.Line(L"error: cannot run %s (code %lu)", exe.c_str(), GetLastError());
        return false;
    }
    const DWORD wait = WaitForSingleObject(pi.hProcess, timeoutMs);
    if (wait == WAIT_OBJECT_0)
        GetExitCodeProcess(pi.hProcess, &exitCode);
    else
        log.Line(L"warning: %s did not finish within %lu ms", exe.c_str(), timeoutMs);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return wait == WAIT_OBJECT_0;
}

bool LaunchDetached(const std::wstring& exe, const std::wstring& args)
{
    std::wstring cmdLine = L"\"" + exe + L"\"";
    if (!args.empty())
        cmdLine += L" " + args;
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(exe.c_str(), cmdLine.data(), nullptr, nullptr, FALSE,
                        0, nullptr, nullptr, &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

bool HttpsGet(const wchar_t* host, const wchar_t* path, std::vector<BYTE>& out,
              DWORD maxBytes, Logger& log, std::atomic<bool>* cancel)
{
    out.clear();

    // AUTOMATIC_PROXY_CONFIG honours the user's system proxy (Win 8.1+; this
    // installer already requires Windows 11).
    HINTERNET session = WinHttpOpen(L"PSCam4Win-Setup",
                                    WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY_CONFIG,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        log.Line(L"download: WinHttpOpen failed (code %lu)", GetLastError());
        return false;
    }
    // resolve / connect / send / receive -- a dead network fails in seconds
    // instead of hanging the progress page.
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);

    bool ok = false;
    HINTERNET connect = WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connect
        ? WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                             WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE)
        : nullptr;

    if (request &&
        WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr))
    {
        DWORD status = 0, statusSize = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize,
                            WINHTTP_NO_HEADER_INDEX);
        if (status == HTTP_STATUS_OK)
        {
            ok = true;
            for (;;)
            {
                if (cancel && cancel->load())
                {
                    ok = false;
                    break;
                }
                DWORD avail = 0;
                if (!WinHttpQueryDataAvailable(request, &avail))
                {
                    log.Line(L"download: read failed (code %lu)", GetLastError());
                    ok = false;
                    break;
                }
                if (avail == 0)   // end of body
                    break;
                if (out.size() + avail > maxBytes)
                {
                    log.Line(L"download: https://%s%s exceeds the %lu byte cap -- refused",
                             host, path, maxBytes);
                    ok = false;
                    break;
                }
                const size_t old = out.size();
                out.resize(old + avail);
                DWORD got = 0;
                if (!WinHttpReadData(request, out.data() + old, avail, &got))
                {
                    log.Line(L"download: read failed (code %lu)", GetLastError());
                    ok = false;
                    break;
                }
                out.resize(old + got);
            }
        }
        else
            log.Line(L"download: https://%s%s returned HTTP %lu", host, path, status);
    }
    else
        log.Line(L"download: cannot reach https://%s%s (code %lu)", host, path,
                 GetLastError());

    if (request)
        WinHttpCloseHandle(request);
    if (connect)
        WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (ok && out.empty())
        ok = false;   // an empty 200 body is never a valid blob
    if (!ok)
        out.clear();
    return ok;
}

} // namespace ops
