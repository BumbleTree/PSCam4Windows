#include "InstallSteps.h"

#include <bcrypt.h>

#include "Payload.h"
#include "SetupLog.h"
#include "installer_resource.h"
#include "..\res\version.h"
#include "..\common\DeviceInterfaceGuids.h"

namespace
{

constexpr wchar_t kArpKeyPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PSCam4Win";
constexpr wchar_t kLegacyArpKeyPath[] =
    L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PS3EyeVCam";
constexpr wchar_t kSettingsKeyPath[] = L"SOFTWARE\\PSCam4Win";
constexpr wchar_t kLegacySettingsKeyPath[] = L"SOFTWARE\\PS3EyeVCam";
constexpr wchar_t kTaskName[] = L"PSCam4Win";
constexpr wchar_t kLegacyTaskName[] = L"PS3EyeVCam";
// Stop order (dependents first); ctx.serviceWasRunning uses the same indices.
const wchar_t* const kServices[2] = { L"FrameServerMonitor", L"FrameServer" };
const wchar_t* const kFwFiles[2] = { L"firmware.bin", L"startup.bin" };

// Auto-download sources for the PS4 camera blobs. firmware.bin is the ONLY
// supported OV580 firmware: the FINAL build (byte-identical across PS4 system
// 6.00-7.02, shipped by the OrbisEyeCam project). Unlike older builds it
// survives close->reopen, so tray restarts never need a replug -- which is why
// it is the only one accepted. startup.bin serves the optional RAW diagnostic
// mode.
// Every candidate is SHA-1-pinned below -- the firmware pin equals the
// kKnownFirmware table the app itself enforces before any USB upload -- so a
// moved, updated, or tampered repo can only make the download fail, never
// install different bytes. An existing firmware.bin that does NOT match the
// pin (an outdated build from an earlier install, or unknown bytes) is
// REPLACED, so upgrades always land on the supported build.
const wchar_t kFwHost[] = L"raw.githubusercontent.com";
const wchar_t* const kFwPaths[2][2] = {
    { L"/psxdev/OrbisEyeCam/master/bin/firmware.bin",
      L"/Hackinside/PS4-CAMERA-DRIVERS/master/firmware.bin" },
    { L"/ps4eye/ps4eye/master/binaries/startup.bin",
      nullptr },
};
const char* const kFwSha1[2] = {
    "8b8a6621d358f6782c2e3b31aac2cdfb25102c00",   // firmware.bin (final), 68032 bytes
    "48d103615008f73a286014ee5408ea9617bb1d3d",   // startup.bin,          59204 bytes
};

// Creates every missing parent directory of `filePath`.
void MakeParents(const std::wstring& filePath)
{
    for (size_t i = 3; i < filePath.size(); ++i)   // skip "C:\"
    {
        if (filePath[i] == L'\\')
            CreateDirectoryW(filePath.substr(0, i).c_str(), nullptr);
    }
}

// True when `path` exists and its whole-file SHA-1 equals `sha1` (lowercase
// hex). Used to decide whether a cached blob is the supported build.
bool FileMatchesSha1(const std::wstring& path, const char* sha1);

// Lowercase-hex BCrypt SHA-1 ("" on failure). The firmware pipeline pins
// SHA-1 end-to-end: the same digest is re-checked by the app before any USB
// upload (ps4::Sha1Hex in transports\usb_ps4\Ps4Firmware.cpp).
std::string Sha1Hex(const BYTE* data, size_t len)
{
    std::string hex;
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA1_ALGORITHM, nullptr, 0) != 0)
        return hex;

    BCRYPT_HASH_HANDLE hash = nullptr;
    BYTE digest[20];
    if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
    {
        if (BCryptHashData(hash, const_cast<PUCHAR>(data), (ULONG)len, 0) == 0 &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
        {
            static const char* h = "0123456789abcdef";
            hex.resize(40);
            for (int i = 0; i < 20; ++i)
            {
                hex[i * 2]     = h[digest[i] >> 4];
                hex[i * 2 + 1] = h[digest[i] & 0xF];
            }
        }
        BCryptDestroyHash(hash);
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hex;
}

bool FileMatchesSha1(const std::wstring& path, const char* sha1)
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    std::vector<BYTE> data;
    LARGE_INTEGER sz{};
    if (GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (16 << 20))
    {
        data.resize((size_t)sz.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, data.data(), (DWORD)data.size(), &got, nullptr) || got != data.size())
            data.clear();
    }
    CloseHandle(f);
    return !data.empty() && Sha1Hex(data.data(), data.size()) == sha1;
}

} // namespace

const DriverDef kDrivers[3] = {
    { L"PS3 Eye",    L"usb_device.inf",    L"usb_device.cer",    L"PnpOem_usb_device",
      IDR_INF_PS3,    IDR_CAT_PS3,    IDR_CER_PS3 },
    { L"PS2 EyeToy", L"eyetoy_device.inf", L"eyetoy_device.cer", L"PnpOem_eyetoy_device",
      IDR_INF_EYETOY, IDR_CAT_EYETOY, IDR_CER_EYETOY },
    { L"PS4 Camera", L"ps4cam_device.inf", L"ps4cam_device.cer", L"PnpOem_ps4cam_device",
      IDR_INF_PS4,    IDR_CAT_PS4,    IDR_CER_PS4 },
};

// kDrivers and kCameraInterfaceGuids are indexed together, PS3 / EyeToy / PS4.
static_assert(kCameraInterfaceGuidCount == 3,
              "kDrivers is indexed into kCameraInterfaceGuids -- keep them in step");

bool DriverSelected(const InstallOptions& opts, int index)
{
    switch (index)
    {
    case 0:  return opts.compPs3;
    case 1:  return opts.compEyeToy;
    case 2:  return opts.compPs4;
    default: return false;
    }
}

namespace
{

// ---------------------------------------------------------------- install steps

// Shared by install and uninstall. Recording prevTrayPath is inert during
// uninstall: RunLinear never unwinds, so Undo does not fire there.
class StopTrayStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Stopping the camera host app"; }

    bool Run(SetupContext& ctx) override
    {
        const std::wstring installedTray = ops::JoinPath(ctx.installDir, payload::kTrayExeName);
        if (ops::PathExists(installedTray))
            ctx.prevTrayPath = installedTray;

        if (!ops::CloseTrayGracefully(5000, *ctx.log))
            ctx.log->Line(L"tray app did not close in time -- terminating it");
        ops::KillProcessByName(payload::kTrayExeName, *ctx.log);
        ops::KillProcessByName(L"PS3EyeVCamTray.exe", *ctx.log);
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        if (!ctx.prevTrayPath.empty() && ops::PathExists(ctx.prevTrayPath))
            ops::LaunchDetached(ctx.prevTrayPath, L"");
    }
};

// Shared by install and uninstall.
class StopServicesStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Stopping the Windows camera services"; }

    bool Run(SetupContext& ctx) override
    {
        // Non-fatal like the old installer: a service that refuses to stop
        // surfaces later as a locked-file error with a clearer message.
        for (int i = 0; i < 2; ++i)
            ops::StopServiceByName(kServices[i], ctx.serviceWasRunning[i], 15000, *ctx.log);
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        for (int i = 1; i >= 0; --i)
            if (ctx.serviceWasRunning[i])
                ops::StartServiceByName(kServices[i], *ctx.log);
    }
};

class MigrateLegacyStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Upgrading the legacy PS3EyeVCam install"; }

    bool Applies(const SetupContext& /*ctx*/) const override
    {
        return ops::ScheduledTaskExists(kLegacyTaskName) ||
               ops::RegKeyExists(HKEY_LOCAL_MACHINE, kLegacySettingsKeyPath) ||
               ops::PathExists(ops::LegacyInstallDir());
    }

    bool Run(SetupContext& ctx) override
    {
        // Removing the deprecated product is one-way by design; every action
        // is best-effort and logged, exactly like the old install.bat.
        if (ops::ScheduledTaskExists(kLegacyTaskName))
        {
            ctx.log->Line(L"removing the old logon task");
            ops::DeleteScheduledTask(kLegacyTaskName, *ctx.log);
        }

        const std::wstring oldDll =
            ops::JoinPath(ops::LegacyInstallDir(), L"PS3EyeVCam.dll");
        if (ops::PathExists(oldDll))
        {
            ctx.log->Line(L"unregistering the old media source DLL");
            if (FAILED(ops::RegisterComDll(oldDll, true, *ctx.log)))
                ops::DeleteVCamClsids(*ctx.log);   // shared CLSIDs across the rebrand
        }

        if (ops::RegKeyExists(HKEY_LOCAL_MACHINE, kLegacySettingsKeyPath))
        {
            ctx.log->Line(L"migrating saved camera settings to PSCam4Win");
            if (ops::CopyRegTree(HKEY_LOCAL_MACHINE, kLegacySettingsKeyPath,
                                 kSettingsKeyPath) == ERROR_SUCCESS)
                RegDeleteTreeW(HKEY_LOCAL_MACHINE, kLegacySettingsKeyPath);
        }

        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kLegacyArpKeyPath);

        if (ops::PathExists(ops::LegacyInstallDir()))
        {
            ctx.log->Line(L"removing the old install folder");
            ops::DeleteDirectoryValidated(ops::LegacyInstallDir(), *ctx.log);
        }
        return true;
    }
};

class CreateDirsStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Creating the install directories"; }

    bool Run(SetupContext& ctx) override
    {
        std::wstring dirs[3];
        Dirs(ctx, dirs);
        for (int i = 0; i < 3; ++i)
        {
            const bool existed = ops::PathExists(dirs[i]);
            if (!ops::EnsureDir(dirs[i]))
            {
                ctx.log->Line(L"error: cannot create %s (code %lu)",
                              dirs[i].c_str(), GetLastError());
                return false;
            }
            ctx.dirCreated[i] = !existed;
        }
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        std::wstring dirs[3];
        Dirs(ctx, dirs);
        // Deepest first; RemoveDirectory only succeeds when empty, which is
        // exactly right (never destroys files someone else put there).
        const int order[3] = { 1, 0, 2 };
        for (const int i : order)
            if (ctx.dirCreated[i])
                RemoveDirectoryW(dirs[i].c_str());
    }

private:
    // ctx.dirCreated is indexed by position, so Run and Undo must build this
    // list identically -- they share one builder to guarantee it.
    static void Dirs(const SetupContext& ctx, std::wstring out[3])
    {
        out[0] = ctx.installDir;
        out[1] = ops::JoinPath(ctx.installDir, L"driver");
        out[2] = ops::ProgramDataDir();
    }
};

class ExtractPayloadStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Extracting the program files"; }

    bool Run(SetupContext& ctx) override
    {
        size_t count = 0;
        const payload::Item* items = payload::Items(count);
        for (size_t i = 0; i < count; ++i)
        {
            const payload::Item& item = items[i];
            if (!ItemSelected(ctx.opts, item.comp))
                continue;

            const BYTE* data = nullptr;
            DWORD size = 0;
            if (!payload::GetResourceBytes(item.resId, data, size))
            {
                ctx.log->Line(L"error: embedded payload %s is missing from this build",
                              item.relTarget);
                return false;
            }

            const std::wstring target = ops::JoinPath(ctx.installDir, item.relTarget);
            if (!BackupExisting(ctx, target, item.relTarget))
                return false;
            if (!payload::WriteFileVerified(target, data, size, *ctx.log))
                return false;
            ctx.filesWritten.push_back(target);
            ctx.log->Line(L"installed %s", item.relTarget);
        }

        // A bat-era install left uninstall.bat behind; quarantine it so a
        // successful install removes it and a rollback puts it back.
        const std::wstring staleBat = ops::JoinPath(ctx.installDir, L"uninstall.bat");
        if (ops::PathExists(staleBat) && !BackupExisting(ctx, staleBat, L"uninstall.bat"))
            return false;

        // The setup exe doubles as the uninstaller.
        const std::wstring setupTarget =
            ops::JoinPath(ctx.installDir, L"PSCam4Win-Setup.exe");
        if (_wcsicmp(setupTarget.c_str(), ctx.setupExePath.c_str()) != 0)
        {
            if (!BackupExisting(ctx, setupTarget, L"PSCam4Win-Setup.exe"))
                return false;
            if (!CopyFileW(ctx.setupExePath.c_str(), setupTarget.c_str(), FALSE))
            {
                ctx.log->Line(L"error: cannot copy the setup program into place (code %lu)",
                              GetLastError());
                return false;
            }
            // The installed uninstaller must never carry mark-of-the-web.
            DeleteFileW((setupTarget + L":Zone.Identifier").c_str());
            ctx.filesWritten.push_back(setupTarget);
            ctx.log->Line(L"installed PSCam4Win-Setup.exe (uninstaller)");
        }
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        for (auto it = ctx.filesWritten.rbegin(); it != ctx.filesWritten.rend(); ++it)
        {
            SetFileAttributesW(it->c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(it->c_str());
        }
        for (auto it = ctx.backups.rbegin(); it != ctx.backups.rend(); ++it)
        {
            MakeParents(it->first);
            MoveFileExW(it->second.c_str(), it->first.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
        }
    }

private:
    static bool ItemSelected(const InstallOptions& opts, Component comp)
    {
        switch (comp)
        {
        case Component::Core:         return true;
        case Component::Ps3Driver:    return opts.compPs3;
        case Component::EyeToyDriver: return opts.compEyeToy;
        case Component::Ps4Driver:    return opts.compPs4;
        default:                      return false;
        }
    }

    static bool BackupExisting(SetupContext& ctx, const std::wstring& target,
                               const wchar_t* relTarget)
    {
        if (!ops::PathExists(target))
            return true;
        if (ctx.backupDir.empty())
        {
            wchar_t tempDir[MAX_PATH] = {};
            GetTempPathW(_countof(tempDir), tempDir);
            wchar_t leaf[64];
            swprintf_s(leaf, L"PSCam4Win-backup-%lu", GetCurrentProcessId());
            ctx.backupDir = ops::JoinPath(tempDir, leaf);
        }
        const std::wstring backupPath = ops::JoinPath(ctx.backupDir, relTarget);
        MakeParents(backupPath);
        if (!MoveFileExW(target.c_str(), backupPath.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        {
            ctx.log->Line(L"error: cannot move the existing %s aside (code %lu). "
                          L"Close every app that is using the camera and retry.",
                          relTarget, GetLastError());
            return false;
        }
        ctx.backups.emplace_back(target, backupPath);
        return true;
    }
};

// The virtual-camera teardown, in one place: the install rollback and the
// uninstall step below both run exactly this.
void UnregisterVCam(SetupContext& ctx)
{
    const std::wstring dll = ops::JoinPath(ctx.installDir, payload::kDllName);
    if (ops::PathExists(dll))
        ops::RegisterComDll(dll, true, *ctx.log);
    // Belt and braces: the DLL's own unregister also deletes these trees.
    ops::DeleteVCamClsids(*ctx.log);
}

class RegisterComStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Registering the virtual camera"; }

    bool Run(SetupContext& ctx) override
    {
        const std::wstring dll = ops::JoinPath(ctx.installDir, payload::kDllName);
        const HRESULT hr = ops::RegisterComDll(dll, false, *ctx.log);
        if (FAILED(hr))
        {
            ctx.log->Line(L"error: virtual camera registration failed (hr=0x%08X). "
                          L"Make sure Windows Media Foundation is intact.", hr);
            return false;
        }
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        UnregisterVCam(ctx);
    }
};

// Kept next to its counterpart above; used by the uninstall list.
class UnregisterComStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Unregistering the virtual camera"; }

    bool Run(SetupContext& ctx) override
    {
        UnregisterVCam(ctx);
        return true;
    }
};

class InstallCertsStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Trusting the driver-signing certificates"; }

    bool Applies(const SetupContext& ctx) const override
    {
        return ctx.opts.compPs3 || ctx.opts.compEyeToy || ctx.opts.compPs4;
    }

    bool Run(SetupContext& ctx) override
    {
        for (int i = 0; i < 3; ++i)
        {
            if (!DriverSelected(ctx.opts, i))
                continue;
            const BYTE* der = nullptr;
            DWORD len = 0;
            if (!payload::GetResourceBytes(kDrivers[i].cerRes, der, len))
            {
                ctx.log->Line(L"error: embedded certificate for the %s driver is missing",
                              kDrivers[i].displayName);
                return false;
            }
            BYTE thumb[20];
            if (ops::ThumbprintOfCertBytes(der, len, thumb))
            {
                bool duplicate = false;
                for (const CertRecord& rec : ctx.certsAdded)
                    duplicate |= memcmp(rec.thumbprint, thumb, 20) == 0;
                if (duplicate)
                    continue;   // the three .cer files can share one certificate
            }
            ctx.log->Line(L"installing the %s driver certificate", kDrivers[i].displayName);
            CertRecord rec;
            if (!ops::AddCertToSystemStores(der, len, rec, *ctx.log))
                return false;
            ctx.certsAdded.push_back(rec);
        }
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        for (auto it = ctx.certsAdded.rbegin(); it != ctx.certsAdded.rend(); ++it)
            if (it->addedToRoot || it->addedToTrustedPub)
                ops::RemoveCertByThumbprint(it->thumbprint, it->addedToRoot,
                                            it->addedToTrustedPub, *ctx.log);
    }
};

class InstallDriversStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Installing the WinUSB camera drivers"; }

    bool Applies(const SetupContext& ctx) const override
    {
        return ctx.opts.compPs3 || ctx.opts.compEyeToy || ctx.opts.compPs4;
    }

    bool Run(SetupContext& ctx) override
    {
        for (int i = 0; i < 3; ++i)
        {
            if (!DriverSelected(ctx.opts, i))
                continue;

            // What is published for this camera before we touch anything. Only
            // meaningful once compared with what pnputil publishes below.
            std::wstring existingOem;
            ops::FindPublishedInfName(kDrivers[i].infLeaf, kCameraInterfaceGuids[i],
                                      existingOem);

            const std::wstring inf = ops::JoinPath(
                ctx.installDir, (std::wstring(L"driver\\") + kDrivers[i].infLeaf).c_str());
            ctx.log->Line(L"installing the %s driver...", kDrivers[i].displayName);

            std::wstring output;
            const DWORD code =
                ops::RunPnputil(L"/add-driver \"" + inf + L"\" /install", output, *ctx.log);
            if (code == 0)
            {
                ctx.log->Line(L"%s driver installed", kDrivers[i].displayName);
            }
            else if (code == 259)   // ERROR_NO_MORE_ITEMS: staged, no device present
            {
                ctx.stagedDriver[i] = true;
                ctx.log->Line(L"%s driver staged -- it binds automatically when the "
                              L"camera is plugged in", kDrivers[i].displayName);
            }
            else if (code == 3010)  // ERROR_SUCCESS_REBOOT_REQUIRED
            {
                ctx.rebootRequired = true;
                ctx.log->Line(L"%s driver installed -- Windows wants a reboot before "
                              L"the camera works", kDrivers[i].displayName);
            }
            else
            {
                ctx.log->Line(L"error: %s driver installation failed (pnputil exit "
                              L"code %lu)", kDrivers[i].displayName, code);
                if (!output.empty())
                    ctx.log->Line(L"pnputil output:\r\n%s", output.c_str());
                ctx.log->Line(L"check that Secure Boot policy allows the self-signed "
                              L"driver certificate");
                return false;
            }

            // Which package did we just add? pnputil says so exactly; the
            // driver-store scan is only a fallback, and it cannot tell one of our
            // packages from another of ours left by an earlier release.
            if (!ops::PublishedNameFromPnputilOutput(output, ctx.oemInfName[i]))
                ops::FindPublishedInfName(kDrivers[i].infLeaf, kCameraInterfaceGuids[i],
                                          ctx.oemInfName[i]);

            // "Pre-existing" means pnputil re-used what was already published, so
            // this install added nothing to retract. Deciding that BEFORE the run --
            // on nothing but "some package of ours exists" -- suppressed the rollback
            // of a package we really had just added, because installs from earlier
            // releases stay in the store and answer to the same name and GUID.
            ctx.driverWasPreexisting[i] =
                !ctx.oemInfName[i].empty() &&
                _wcsicmp(ctx.oemInfName[i].c_str(), existingOem.c_str()) == 0;

            if (!ctx.oemInfName[i].empty())
                ctx.log->Line(L"%s driver package published as %s%s",
                              kDrivers[i].displayName, ctx.oemInfName[i].c_str(),
                              ctx.driverWasPreexisting[i] ? L" (already present)" : L"");
        }
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        for (int i = 2; i >= 0; --i)
        {
            if (ctx.driverWasPreexisting[i] || ctx.oemInfName[i].empty())
                continue;   // never retract a package that predates this install
            std::wstring output;
            ops::RunPnputil(L"/delete-driver " + ctx.oemInfName[i] + L" /uninstall /force",
                            output, *ctx.log);
        }
    }
};

class StageFirmwareStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Preparing the PS4 firmware cache"; }

    bool Run(SetupContext& ctx) override
    {
        // PSCam4Win ships NO Sony firmware. A verified copy the user placed
        // next to the setup exe wins (works offline); anything still missing
        // -- or an OUTDATED/unknown cached firmware.bin -- is (re)staged from
        // the pinned download below. The app SHA-1-verifies the firmware
        // again before any USB upload.
        const std::wstring fwDir = ops::ProgramDataDir();

        for (int i = 0; i < 2; ++i)
        {
            const std::wstring src = ops::JoinPath(ctx.sourceDir, kFwFiles[i]);
            const std::wstring dst = ops::JoinPath(fwDir, kFwFiles[i]);
            if (ops::PathExists(src) && !FileMatchesSha1(dst, kFwSha1[i]) &&
                FileMatchesSha1(src, kFwSha1[i]))
            {
                if (CopyFileW(src.c_str(), dst.c_str(), FALSE))
                {
                    ctx.fwCopied[i] = true;
                    ctx.log->Line(L"staged %s into %s", kFwFiles[i], fwDir.c_str());
                }
            }
        }

        // Auto-download into the cache the app reads. Best-effort by design:
        // the PS4 camera is optional, so an offline install must still
        // succeed -- the app just keeps reporting the missing firmware until
        // it appears. Only bytes matching the pinned SHA-1s are ever written.
        if (ctx.opts.compPs4)
        {
            for (int i = 0; i < 2; ++i)
            {
                const std::wstring dst = ops::JoinPath(fwDir, kFwFiles[i]);
                if (FileMatchesSha1(dst, kFwSha1[i]))
                    continue;   // already the supported bytes
                if (ops::PathExists(dst))
                    ctx.log->Line(L"%s is not the supported build -- replacing",
                                  kFwFiles[i]);
                for (const wchar_t* path : kFwPaths[i])
                {
                    if (!path)
                        break;
                    ctx.log->Line(L"downloading %s from https://%s%s",
                                  kFwFiles[i], kFwHost, path);
                    std::vector<BYTE> blob;
                    if (!ops::HttpsGet(kFwHost, path, blob, 4u << 20, *ctx.log,
                                       ctx.cancel))
                        continue;   // unreachable or oversized -- try the mirror
                    if (Sha1Hex(blob.data(), blob.size()) != kFwSha1[i])
                    {
                        ctx.log->Line(L"%s from https://%s%s failed SHA-1 "
                                      L"verification -- discarded",
                                      kFwFiles[i], kFwHost, path);
                        continue;
                    }
                    if (payload::WriteFileVerified(dst, blob.data(),
                                                   (DWORD)blob.size(), *ctx.log))
                    {
                        ctx.fwCopied[i] = true;
                        ctx.log->Line(L"%s verified (SHA-1) and installed into %s",
                                      kFwFiles[i], fwDir.c_str());
                    }
                    break;   // a write failure is a disk problem; mirrors won't help
                }
            }
        }

        if (ctx.opts.compPs4 && !ops::PathExists(ops::JoinPath(fwDir, kFwFiles[0])))
        {
            ctx.log->Line(L"note: firmware.bin could not be staged, so the PS4 camera "
                          L"stays inactive.");
            ctx.log->Line(L"      Re-run setup online, or place firmware.bin in:");
            ctx.log->Line(L"      %s", fwDir.c_str());
            ctx.log->Line(L"      (see the README). PS3 and PS2 cameras need nothing.");
        }
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        for (int i = 0; i < 2; ++i)
            if (ctx.fwCopied[i])
                DeleteFileW(ops::JoinPath(ops::ProgramDataDir(), kFwFiles[i]).c_str());
    }
};

class SeedDefaultsStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Writing default settings"; }

    bool Run(SetupContext& ctx) override
    {
        ctx.settingsKeyExisted = ops::RegKeyExists(HKEY_LOCAL_MACHINE, kSettingsKeyPath);
        const std::wstring tray = ops::JoinPath(ctx.installDir, payload::kTrayExeName);
        DWORD exitCode = 0;
        if (!ops::RunAndWait(tray, L"--seed-defaults", exitCode, 15000, *ctx.log) ||
            exitCode != 0)
            ctx.log->Line(L"warning: could not seed default settings (the app seeds "
                          L"them on first run)");
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        if (!ctx.settingsKeyExisted)
            RegDeleteTreeW(HKEY_LOCAL_MACHINE, kSettingsKeyPath);
    }
};

class EnableAutostartStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Registering start-at-logon"; }

    bool Applies(const SetupContext& ctx) const override { return ctx.opts.autostart; }

    bool Run(SetupContext& ctx) override
    {
        ctx.autostartTaskExisted = ops::ScheduledTaskExists(kTaskName);
        const std::wstring tray = ops::JoinPath(ctx.installDir, payload::kTrayExeName);
        DWORD exitCode = 0;
        if (!ops::RunAndWait(tray, L"--enable-autostart", exitCode, 15000, *ctx.log) ||
            exitCode != 0)
            ctx.log->Line(L"warning: could not create the logon task -- toggle "
                          L"\"Start with Windows\" in the app");
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        if (!ctx.autostartTaskExisted)
            ops::DeleteScheduledTask(kTaskName, *ctx.log);
    }
};

class WriteArpStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Registering with Apps && Features"; }

    bool Run(SetupContext& ctx) override
    {
        SnapshotArp(ctx.arpSnapshot);

        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kArpKeyPath, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        {
            ctx.log->Line(L"error: cannot create the Apps & Features entry (code %lu)",
                          GetLastError());
            return false;
        }

        auto setString = [key](const wchar_t* name, const std::wstring& value) {
            RegSetValueExW(key, name, 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(value.c_str()),
                           static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        };
        auto setDword = [key](const wchar_t* name, DWORD value) {
            RegSetValueExW(key, name, 0, REG_DWORD,
                           reinterpret_cast<const BYTE*>(&value), sizeof(value));
        };

        const std::wstring setupExe =
            ops::JoinPath(ctx.installDir, L"PSCam4Win-Setup.exe");
        setString(L"DisplayName", L"PSCam4Win Virtual Camera");
        setString(L"DisplayVersion", L"" PSCAM_VERSION_DISPLAY);
        setString(L"Publisher", L"PSCam4Win");
        setString(L"InstallLocation", ctx.installDir);
        setString(L"DisplayIcon", ops::JoinPath(ctx.installDir, payload::kTrayExeName));
        setString(L"UninstallString", L"\"" + setupExe + L"\" --uninstall");
        setDword(L"NoModify", 1);
        setDword(L"NoRepair", 1);
        setDword(L"EstimatedSize", payload::TotalPayloadSizeKb());
        setDword(L"VersionMajor", PSCAM_VERSION_MAJOR);
        setDword(L"VersionMinor", PSCAM_VERSION_MINOR);
        for (int i = 0; i < 3; ++i)
            if (!ctx.oemInfName[i].empty())
                setString(kDrivers[i].arpOemValue, ctx.oemInfName[i]);

        RegCloseKey(key);
        return true;
    }

    void Undo(SetupContext& ctx) noexcept override
    {
        RegDeleteTreeW(HKEY_LOCAL_MACHINE, kArpKeyPath);
        if (!ctx.arpSnapshot.existed)
            return;
        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kArpKeyPath, 0, nullptr, 0,
                            KEY_WRITE, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return;
        for (const ArpSnapshot::Value& v : ctx.arpSnapshot.values)
            RegSetValueExW(key, v.name.c_str(), 0, v.type, v.data.data(),
                           static_cast<DWORD>(v.data.size()));
        RegCloseKey(key);
    }

private:
    static void SnapshotArp(ArpSnapshot& snapshot)
    {
        snapshot = ArpSnapshot{};
        HKEY key = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kArpKeyPath, 0, KEY_READ, &key) !=
            ERROR_SUCCESS)
            return;
        snapshot.existed = true;
        for (DWORD index = 0;; ++index)
        {
            wchar_t name[256];
            DWORD nameLen = _countof(name);
            DWORD type = 0;
            BYTE data[2048];
            DWORD dataLen = sizeof(data);
            const LSTATUS status = RegEnumValueW(key, index, name, &nameLen, nullptr,
                                                 &type, data, &dataLen);
            if (status != ERROR_SUCCESS)
                break;
            ArpSnapshot::Value value;
            value.name = name;
            value.type = type;
            value.data.assign(data, data + dataLen);
            snapshot.values.push_back(std::move(value));
        }
        RegCloseKey(key);
    }
};

// Shared by install and uninstall.
class RestartServicesStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Restarting the Windows camera services"; }

    bool Run(SetupContext& ctx) override
    {
        ops::StartServiceByName(L"FrameServer", *ctx.log);
        ops::StartServiceByName(L"FrameServerMonitor", *ctx.log);
        return true;
    }
};

class LaunchTrayStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Starting the camera host app"; }

    bool Applies(const SetupContext& ctx) const override { return ctx.opts.launchAfter; }

    bool Run(SetupContext& ctx) override
    {
        const std::wstring tray = ops::JoinPath(ctx.installDir, payload::kTrayExeName);
        if (!ops::LaunchDetached(tray, L""))
            ctx.log->Line(L"warning: could not start the tray app -- launch it from "
                          L"the Start menu");
        return true;
    }
};

// ---------------------------------------------------------------- uninstall steps

class UDeleteTaskStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Removing the logon task"; }

    bool Run(SetupContext& ctx) override
    {
        return ops::DeleteScheduledTask(kTaskName, *ctx.log);
    }
};

class UDeleteDriversStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Removing the WinUSB camera drivers"; }

    bool Run(SetupContext& ctx) override
    {
        bool allOk = true;
        for (int i = 0; i < 3; ++i)
        {
            // Prefer the oem name recorded at install time; fall back to a
            // driver-store scan (covers bat-era installs).
            std::wstring oem = ctx.arpState.oemInf[i];
            if (oem.empty())
                ops::FindPublishedInfName(kDrivers[i].infLeaf, kCameraInterfaceGuids[i], oem);
            if (oem.empty())
            {
                ctx.log->Line(L"%s driver is not installed -- skipping",
                              kDrivers[i].displayName);
                continue;
            }

            std::wstring output;
            DWORD code = ops::RunPnputil(L"/delete-driver " + oem + L" /uninstall /force",
                                         output, *ctx.log);
            if (code != 0 && code != 3010)
            {
                // The recorded name may be stale after a re-publish; rescan once.
                std::wstring rescanned;
                if (ops::FindPublishedInfName(kDrivers[i].infLeaf,
                                              kCameraInterfaceGuids[i], rescanned) &&
                    _wcsicmp(rescanned.c_str(), oem.c_str()) != 0)
                    code = ops::RunPnputil(L"/delete-driver " + rescanned +
                                           L" /uninstall /force", output, *ctx.log);
            }

            if (code == 0)
                ctx.log->Line(L"%s driver removed", kDrivers[i].displayName);
            else if (code == 3010)
            {
                ctx.rebootRequired = true;
                ctx.log->Line(L"%s driver removed -- a reboot completes the cleanup",
                              kDrivers[i].displayName);
            }
            else
            {
                ctx.log->Line(L"warning: could not remove the %s driver (pnputil exit "
                              L"code %lu)", kDrivers[i].displayName, code);
                allOk = false;
            }
        }
        return allOk;
    }
};

class URemoveCertsStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Removing the driver-signing certificates"; }

    bool Run(SetupContext& ctx) override
    {
        std::vector<std::vector<BYTE>> seen;
        bool allOk = true;
        for (int i = 0; i < 3; ++i)
        {
            BYTE thumb[20];
            const std::wstring cerFile = ops::JoinPath(
                ctx.installDir, (std::wstring(L"driver\\") + kDrivers[i].cerLeaf).c_str());
            bool haveThumb = ops::ThumbprintOfCertFile(cerFile, thumb);
            if (!haveThumb)
            {
                const BYTE* der = nullptr;
                DWORD len = 0;
                if (payload::GetResourceBytes(kDrivers[i].cerRes, der, len))
                    haveThumb = ops::ThumbprintOfCertBytes(der, len, thumb);
            }
            if (!haveThumb)
                continue;

            bool duplicate = false;
            for (const auto& previous : seen)
                duplicate |= memcmp(previous.data(), thumb, 20) == 0;
            if (duplicate)
                continue;
            seen.emplace_back(thumb, thumb + 20);

            allOk &= ops::RemoveCertByThumbprint(thumb, true, true, *ctx.log);
        }
        return allOk;
    }
};

class URemoveDataStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Removing saved settings and firmware"; }

    bool Applies(const SetupContext& ctx) const override { return ctx.opts.removeData; }

    bool Run(SetupContext& ctx) override
    {
        const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kSettingsKeyPath);
        if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
            ctx.log->Line(L"warning: could not delete the settings key (code %ld)", status);
        return ops::DeleteDirectoryValidated(ops::ProgramDataDir(), *ctx.log);
    }
};

class UDeleteArpStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Removing the Apps && Features entry"; }

    bool Run(SetupContext& /*ctx*/) override
    {
        const LSTATUS status = RegDeleteTreeW(HKEY_LOCAL_MACHINE, kArpKeyPath);
        return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
    }
};

class UDeleteInstallDirStep : public Step
{
public:
    const wchar_t* Name() const override { return L"Deleting the program files"; }

    bool Run(SetupContext& ctx) override
    {
        if (!ops::PathExists(ctx.installDir))
            return true;
        if (!ops::DeleteDirectoryValidated(ctx.installDir, *ctx.log))
        {
            ctx.log->Line(L"warning: some files in %s could not be deleted (still in "
                          L"use?) -- remove the folder manually after a reboot",
                          ctx.installDir.c_str());
            return false;
        }
        return true;
    }
};

// ---------------------------------------------------------------- engine

void NotifyProgress(SetupContext& ctx, int done, int total)
{
    if (ctx.notifyWnd)
        PostMessageW(ctx.notifyWnd, WM_APP_PROGRESS, static_cast<WPARAM>(done),
                     static_cast<LPARAM>(total));
}

int CountApplicable(const std::vector<std::unique_ptr<Step>>& steps, const SetupContext& ctx)
{
    int total = 0;
    for (const auto& step : steps)
        if (step->Applies(ctx))
            ++total;
    return total;
}

} // namespace

namespace engine
{

std::vector<std::unique_ptr<Step>> BuildInstallSteps()
{
    std::vector<std::unique_ptr<Step>> steps;
    steps.push_back(std::make_unique<StopTrayStep>());
    steps.push_back(std::make_unique<StopServicesStep>());
    steps.push_back(std::make_unique<MigrateLegacyStep>());
    steps.push_back(std::make_unique<CreateDirsStep>());
    steps.push_back(std::make_unique<ExtractPayloadStep>());
    steps.push_back(std::make_unique<RegisterComStep>());
    steps.push_back(std::make_unique<InstallCertsStep>());
    steps.push_back(std::make_unique<InstallDriversStep>());
    steps.push_back(std::make_unique<StageFirmwareStep>());
    steps.push_back(std::make_unique<SeedDefaultsStep>());
    steps.push_back(std::make_unique<EnableAutostartStep>());
    steps.push_back(std::make_unique<WriteArpStep>());
    steps.push_back(std::make_unique<RestartServicesStep>());
    steps.push_back(std::make_unique<LaunchTrayStep>());
    return steps;
}

// Unprefixed entries are the same classes the install list uses; only the
// U-prefixed ones are uninstall-only.
std::vector<std::unique_ptr<Step>> BuildUninstallSteps()
{
    std::vector<std::unique_ptr<Step>> steps;
    steps.push_back(std::make_unique<StopTrayStep>());
    steps.push_back(std::make_unique<UDeleteTaskStep>());
    steps.push_back(std::make_unique<StopServicesStep>());
    steps.push_back(std::make_unique<UnregisterComStep>());
    steps.push_back(std::make_unique<UDeleteDriversStep>());
    steps.push_back(std::make_unique<URemoveCertsStep>());
    steps.push_back(std::make_unique<URemoveDataStep>());
    steps.push_back(std::make_unique<UDeleteArpStep>());
    steps.push_back(std::make_unique<UDeleteInstallDirStep>());
    steps.push_back(std::make_unique<RestartServicesStep>());
    return steps;
}

EngineResult Run(std::vector<std::unique_ptr<Step>>& steps, SetupContext& ctx)
{
    const int total = CountApplicable(steps, ctx);
    NotifyProgress(ctx, 0, total);

    std::vector<Step*> completed;
    EngineResult result = EngineResult::Success;
    int index = 0;
    for (const auto& step : steps)
    {
        if (!step->Applies(ctx))
            continue;
        if (ctx.cancel && ctx.cancel->load())
        {
            ctx.log->Line(L"cancelled by user");
            result = EngineResult::Cancelled;
            break;
        }
        ++index;
        ctx.log->Line(L"=== (%d/%d) %s", index, total, step->Name());
        if (!step->Run(ctx))
        {
            result = EngineResult::Failed;
            break;
        }
        completed.push_back(step.get());
        NotifyProgress(ctx, index, total);
    }

    if (result != EngineResult::Success)
    {
        ctx.log->Line(result == EngineResult::Cancelled
                          ? L"--- rolling back the cancelled install..."
                          : L"--- install failed, rolling back...");
        for (auto it = completed.rbegin(); it != completed.rend(); ++it)
        {
            ctx.log->Line(L"undo: %s", (*it)->Name());
            (*it)->Undo(ctx);
        }
        ctx.log->Line(L"rollback finished");
    }

    if (!ctx.backupDir.empty())
        ops::DeleteTempDirectory(ctx.backupDir, *ctx.log);

    if (result == EngineResult::Success)
        return ctx.rebootRequired ? EngineResult::SuccessReboot : EngineResult::Success;
    return result;
}

EngineResult RunLinear(std::vector<std::unique_ptr<Step>>& steps, SetupContext& ctx)
{
    const int total = CountApplicable(steps, ctx);
    NotifyProgress(ctx, 0, total);

    int index = 0;
    for (const auto& step : steps)
    {
        if (!step->Applies(ctx))
            continue;
        if (ctx.cancel && ctx.cancel->load())
        {
            ctx.log->Line(L"cancelled by user -- the remaining cleanup was skipped; "
                          L"run the uninstaller again to finish");
            return EngineResult::Cancelled;
        }
        ++index;
        ctx.log->Line(L"=== (%d/%d) %s", index, total, step->Name());
        if (!step->Run(ctx))
            ++ctx.warnings;
        NotifyProgress(ctx, index, total);
    }
    return ctx.rebootRequired ? EngineResult::SuccessReboot : EngineResult::Success;
}

} // namespace engine
