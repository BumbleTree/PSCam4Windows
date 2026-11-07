#include <windows.h>
#include <objbase.h>
#include <winioctl.h>
#include <thread>
#include <chrono>
#include "TestSupport.h"
#include "../installer/Payload.h"
#include "../installer/SetupOps.h"
static int certAdds = 0, certRemoves = 0;
static bool removedRoot = false, removedPublisher = false;
namespace payload {
bool TestCertificateResource(WORD, const BYTE*& data, DWORD& size)
{ static const BYTE bytes[] = { 1 }; data = bytes; size = sizeof(bytes); return true; }
}
namespace ops {
bool TestAddCertificate(const BYTE*, DWORD, CertRecord& record, Logger&)
{ ++certAdds; record.thumbprint[0] = 7; record.addedToRoot = true; return false; }
bool TestRemoveCertificate(const BYTE[20], bool root, bool publisher, Logger&)
{ ++certRemoves; removedRoot = root; removedPublisher = publisher; return true; }
// The fixture certificate is one byte, so it has no real thumbprint.
bool TestThumbprint(const BYTE* data, DWORD size, BYTE out[20])
{ if (!data || !size) return false; memset(out, 0, 20); out[0] = 9; return true; }
}
#define GetResourceBytes TestCertificateResource
#define AddCertToSystemStores TestAddCertificate
#define RemoveCertByThumbprint TestRemoveCertificate
#define ThumbprintOfCertBytes TestThumbprint
#include "../installer/InstallSteps.cpp"
#undef ThumbprintOfCertBytes
#undef GetResourceBytes
#undef AddCertToSystemStores
#undef RemoveCertByThumbprint

class FailingWrite : public Step
{
public:
    std::wstring target;
    bool undoCalled = false;
    bool writeCompleted = false;
    bool holdAfterWrite = false;
    HANDLE held = INVALID_HANDLE_VALUE;
    const wchar_t* Name() const override { return L"fixture partial write"; }
    bool Run(SetupContext& ctx) override
    {
        if (!BackupFile(ctx, target, L"payload.bin", ctx.backups)) return false;
        ctx.filesWritten.push_back(target);
        const BYTE replacement[] = { 9, 8, 7 };
        if (!payload::WriteFileVerified(target, replacement, sizeof(replacement), *ctx.log))
            return false;
        writeCompleted = true;
        if (holdAfterWrite)
        {
            held = CreateFileW(target.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (held == INVALID_HANDLE_VALUE)
                printf("fixture lock error: %lu\n", GetLastError());
        }
        return false;
    }
    void Undo(SetupContext& ctx) noexcept override
    {
        undoCalled = true;
        RestoreFiles(ctx, ctx.filesWritten, ctx.backups);
    }
};

static void TestRollback(Logger& log)
{
    const std::wstring root = ScratchDirectory();
    Check(!root.empty(), "rollback fixture created exclusively");
    if (root.empty()) return;
    const std::wstring target = root + L"\\payload.bin";
    const std::vector<BYTE> original{ 1, 2, 3, 4 };
    for (bool hold : { false, true })
    {
        Check(WriteBytes(target, original), "original payload created");
        SetupContext ctx; ctx.log = &log;
        std::vector<std::unique_ptr<Step>> steps;
        auto step = std::make_unique<FailingWrite>();
        auto* observe = step.get();
        step->target = target; step->holdAfterWrite = hold;
        steps.push_back(std::move(step));
        Check(engine::Run(steps, ctx) == EngineResult::Failed, "partial write fails the transaction");
        Check(observe->writeCompleted, "fault injection occurs after replacing the original bytes");
        Check(observe->undoCalled, "failed step participates in rollback");
        if (!hold)
        {
            Check(ReadBytes(target) == original, "old payload restored after partial write");
            Check(!ctx.rollbackIncomplete, "successful restore has complete recovery state");
            Check(!ops::PathExists(ctx.backupDir), "completed recovery removes scratch backup");
        }
        else
        {
            Check(observe->held != INVALID_HANDLE_VALUE, "fixture locks replacement during recovery");
            Check(ctx.rollbackIncomplete, "failed restore reports incomplete recovery");
            Check(!ctx.backups.empty() && ReadBytes(ctx.backups.front().second) == original,
                  "failed restore retains original backup bytes");
            if (observe->held != INVALID_HANDLE_VALUE) CloseHandle(observe->held);
            ctx.rollbackIncomplete = false;
            RestoreFiles(ctx, ctx.filesWritten, ctx.backups);
            Check(ReadBytes(target) == original && !ctx.rollbackIncomplete,
                  "retained backup can recover once lock is released");
            Check(ops::DeleteTempDirectory(ctx.backupDir, log), "retained fixture backup cleaned safely");
        }
    }
    Check(ops::DeleteTempDirectory(root, log), "rollback fixture cleaned safely");
}

static void TestRegistry()
{
    const std::wstring path = L"SOFTWARE\\PSCam4Win.TestFixture." + std::to_wstring(GetCurrentProcessId());
    HKEY key = nullptr;
    DWORD disposition = 0;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0,
                                            KEY_ALL_ACCESS, nullptr, &key, &disposition);
    if (created != ERROR_SUCCESS)
    {
        printf("[SKIP] registry fixture is not writable (%ld)\n", created);
        ++testSkips; return;
    }
    Check(disposition == REG_CREATED_NEW_KEY, "registry fixture is newly created");
    if (disposition != REG_CREATED_NEW_KEY) { RegCloseKey(key); return; }
    const std::vector<BYTE> original(8192, 0x73);
    Check(RegSetValueExW(key, L"Binary", 0, REG_BINARY, original.data(),
                         static_cast<DWORD>(original.size())) == ERROR_SUCCESS, "large registry value created");
    HKEY child = nullptr;
    Check(RegCreateKeyExW(key, L"Nested\\Empty", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr,
                          &child, nullptr) == ERROR_SUCCESS, "empty nested registry key created");
    if (child) RegCloseKey(child);
    RegCloseKey(key);
    ops::RegistryTree snapshot;
    Check(ops::SnapshotRegistryTree(HKEY_CURRENT_USER, path.c_str(), snapshot) == ERROR_SUCCESS,
          "registry tree snapshot succeeds");
    RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
    RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr);
    const BYTE changed = 0;
    RegSetValueExW(key, L"Unexpected", 0, REG_BINARY, &changed, 1);
    RegCloseKey(key);
    Check(ops::RestoreRegistryTree(HKEY_CURRENT_USER, path.c_str(), snapshot) == ERROR_SUCCESS,
          "registry tree restores after destructive replacement");
    ops::RegistryTree restored;
    Check(ops::SnapshotRegistryTree(HKEY_CURRENT_USER, path.c_str(), restored) == ERROR_SUCCESS &&
          restored.keys.size() == snapshot.keys.size() && !restored.keys.empty() &&
          restored.keys[0].values.size() == 1 && restored.keys[0].values[0].data == original,
          "restoration preserves large values and removes added metadata");
    Check(ops::RegKeyExists(HKEY_CURRENT_USER, (path + L"\\Nested\\Empty").c_str()),
          "restoration preserves empty nested keys");
    Check(RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()) == ERROR_SUCCESS, "registry fixture removed");
}

static bool Junction(const std::wstring& link, const std::wstring& target)
{
    if (!CreateDirectoryW(link.c_str(), nullptr)) return false;
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) return false;
    const std::wstring substitute = L"\\??\\" + target;
    const size_t pathBytes = (substitute.size() + target.size() + 2) * sizeof(wchar_t);
    std::vector<BYTE> buffer(16 + pathBytes, 0);
    auto put16 = [&](size_t offset, size_t value) {
        const WORD word = static_cast<WORD>(value); memcpy(buffer.data() + offset, &word, sizeof(word));
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    memcpy(buffer.data(), &tag, sizeof(tag));
    put16(4, 8 + pathBytes); put16(8, 0); put16(10, substitute.size() * sizeof(wchar_t));
    put16(12, (substitute.size() + 1) * sizeof(wchar_t)); put16(14, target.size() * sizeof(wchar_t));
    memcpy(buffer.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
    memcpy(buffer.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), target.c_str(),
           (target.size() + 1) * sizeof(wchar_t));
    DWORD returned = 0;
    const bool ok = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                                    static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr) != 0;
    CloseHandle(handle);
    return ok;
}

static void TestDeletion(Logger& log)
{
    const std::wstring target = ScratchDirectory(), removable = ScratchDirectory();
    Check(!target.empty() && !removable.empty(), "junction fixtures created exclusively");
    if (target.empty() || removable.empty()) return;
    const std::vector<BYTE> sentinel{ 0x42, 0x19 };
    const std::wstring sentinelPath = target + L"\\sentinel.bin";
    Check(WriteBytes(sentinelPath, sentinel), "outside sentinel created");
    const std::wstring link = removable + L"\\outside";
    if (Junction(link, target))
    {
        Check(ops::DeleteTempDirectory(removable, log), "cleanup removes directory junction");
        Check(ReadBytes(sentinelPath) == sentinel, "junction target sentinel survives cleanup");
    }
    else
    {
        printf("[SKIP] junction creation unavailable\n"); ++testSkips;
        Check(ops::DeleteTempDirectory(removable, log), "unlinked fixture cleanup succeeds");
    }
    Check(ops::DeleteTempDirectory(target, log), "outside fixture cleanup succeeds");
}

static void TestProcess(Logger& log)
{
    const std::wstring exe = ops::ModulePath();
    std::wstring output;
    ULONGLONG before = GetTickCount64();
    Check(ops::RunProcessCaptured(exe, L"--hold-pipe", output, log, nullptr, 150) == ERROR_TIMEOUT &&
          GetTickCount64() - before < 3000, "deadline bounds a child holding stdout open");
    std::atomic<bool> cancel{ false };
    std::thread signal([&] { Sleep(100); cancel.store(true); });
    before = GetTickCount64();
    const DWORD cancelled = ops::RunProcessCaptured(exe, L"--hold-pipe", output, log, &cancel, 5000);
    signal.join();
    Check(cancelled == ERROR_CANCELLED && GetTickCount64() - before < 3000,
          "cancellation interrupts a child holding stdout open");
    Check(ops::RunProcessCaptured(exe, L"--burst", output, log, nullptr, 5000) == 7 &&
          output.size() == 200000, "child output larger than pipe capacity drains without deadlock");
}

static void TestCertificates(Logger& log)
{
    SetupContext ctx; ctx.log = &log;
    InstallCertsStep add;
    URemoveCertsStep remove;
    CertRecord owned; owned.thumbprint[0] = 8; owned.addedToRoot = true;
    ctx.arpState.ownedCertificates.push_back(owned);
    Check(!add.Run(ctx) && certAdds == 1 && ctx.certsAdded.size() == 1 &&
          ctx.certsAdded[0].addedToRoot, "partial certificate addition is journalled before failure");
    add.Undo(ctx);
    Check(certRemoves == 1 && removedRoot && !removedPublisher,
          "certificate rollback removes only the store actually changed");
    certRemoves = 0;
    Check(remove.Run(ctx) && certRemoves == 1 && removedRoot && !removedPublisher,
          "uninstall removes only persisted owned store entries");
    ctx.arpState.ownedCertificates.clear(); certRemoves = 0;
    ctx.arpState.installed = true; ctx.arpState.certOwnershipTracked = true;
    Check(remove.Run(ctx) && certRemoves == 0, "unowned trust survives uninstall");
    ctx.arpState.certOwnershipTracked = false;
    Check(remove.Run(ctx) && certRemoves > 0 && removedRoot && removedPublisher,
          "uninstall cleans up after an install from before ownership records");
}

// The whole-device check reads [Strings] DeviceID through SetupAPI, in both
// encodings the driver store holds: libwdi wrote UTF-16 INFs, ours are ANSI.
static void TestInfDeviceId()
{
    const std::wstring dir = ScratchDirectory();
    Check(!dir.empty(), "INF fixture directory created");
    if (dir.empty()) return;
    auto writeInf = [&](const wchar_t* leaf, const char* deviceId, bool utf16) {
        const std::string body = std::string("[Version]\r\nSignature = \"$Windows NT$\"\r\n"
            "[Strings]\r\nDeviceID = \"") + deviceId + "\"\r\n";
        std::vector<BYTE> bytes;
        if (utf16)
        {
            bytes = { 0xFF, 0xFE };
            for (char c : body) { bytes.push_back(static_cast<BYTE>(c)); bytes.push_back(0); }
        }
        else
            bytes.assign(body.begin(), body.end());
        const std::wstring path = dir + L"\\" + leaf;
        return WriteBytes(path, bytes) ? path : std::wstring();
    };
    const std::wstring whole = writeInf(L"whole.inf", "VID_1415&PID_2000", true);
    const std::wstring iface = writeInf(L"iface.inf", "VID_1415&PID_2000&MI_00", false);
    Check(!whole.empty() && ops::InfDeviceIdIs(whole, L"VID_1415&PID_2000"),
          "a UTF-16 whole-device PS3 Eye INF is recognised");
    Check(!iface.empty() && !ops::InfDeviceIdIs(iface, L"VID_1415&PID_2000"),
          "the MI_00 interface INF is not mistaken for a whole-device one");
    Check(!ops::InfDeviceIdIs(dir + L"\\missing.inf", L"VID_1415&PID_2000"),
          "a missing INF is never reported as a match");
    Logger quiet;
    Check(ops::DeleteTempDirectory(dir, quiet), "INF fixture cleaned safely");
}

int main(int argc, char** argv)
{
    if (argc > 1 && std::string(argv[1]) == "--hold-pipe") { Sleep(10000); return 0; }
    if (argc > 1 && std::string(argv[1]) == "--burst")
    {
        const std::string text(200000, 'x'); fwrite(text.data(), 1, text.size(), stdout); return 7;
    }
    Logger log;
    log.Open(L"build\\tests\\installer-fixtures.log");
    TestRollback(log); TestRegistry(); TestDeletion(log); TestProcess(log); TestCertificates(log);
    TestInfDeviceId();
    std::wstring published;
    Check(ops::PublishedNameFromPnputilOutput(L"Veroeffentlichter Name: oem17.inf\r\n", published) &&
          published == L"oem17.inf", "published-name parsing supports localized labels");
    Check(!ops::PublishedNameFromPnputilOutput(L"Existing: oem17.inf\nPublished: oem42.inf", published) &&
          published.empty(), "published-name parsing refuses ambiguous driver names");
    Check(!ops::PublishedNameFromPnputilOutput(L"unrelated_oem17.inf.bak", published),
          "published-name parsing rejects embedded filename fragments");
    Check(!ops::PublishedInfMatches(L"..\\oem1.inf", L"usb_device.inf", kCameraInterfaceGuids[0]),
          "driver identity rejects paths instead of published leaf names");
    printf("installer: %d failures, %d skips\n", testFailures, testSkips);
    return testFailures ? 1 : 0;
}
