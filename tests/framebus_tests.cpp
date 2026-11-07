#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <aclapi.h>
#include <thread>
#include <algorithm>
#include "TestSupport.h"

static std::wstring TestName(LPCWSTR name)
{
    return L"Local\\PSCam4Win.FrameBusTests." + std::to_wstring(GetCurrentProcessId()) + L"." + (name + 7);
}
static HANDLE createdMap = nullptr;
static HANDLE TestCreateMap(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name)
{
    createdMap = CreateFileMappingW(file, sa, protect, hi, lo, TestName(name).c_str());
    return createdMap;
}
static HANDLE TestOpenMap(DWORD access, BOOL inherit, LPCWSTR name)
{ return OpenFileMappingW(access, inherit, TestName(name).c_str()); }
static HANDLE TestCreateEvent(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{ return CreateEventW(sa, manual, initial, TestName(name).c_str()); }
static HANDLE TestOpenEvent(DWORD access, BOOL inherit, LPCWSTR name)
{ return OpenEventW(access, inherit, TestName(name).c_str()); }
#define CreateFileMappingW TestCreateMap
#define OpenFileMappingW TestOpenMap
#define CreateEventW TestCreateEvent
#define OpenEventW TestOpenEvent
#include "../common/FrameBus.h"
#undef CreateFileMappingW
#undef OpenFileMappingW
#undef CreateEventW
#undef OpenEventW

int main()
{
    framebus::Writer writer;
    Check(writer.Create(0, 640, 480, 30, 1, framebus::kMaxFrameBytes, 0), "valid isolated bus created");
    if (testFailures) return 1;
    framebus::ColdMode mode{ 640, 480, 30, framebus::kFmtYUY2 };
    writer.WriteColdBlock(1, framebus::kFmtYUY2, &mode, 1);
    writer.PublishBlack();
    framebus::Reader reader;
    Check(reader.TryOpen(0), "read-only bus reader opens");
    wchar_t name[64]; ipcnames::Format(name, L".FrameBus", 0);
    HANDLE map = nullptr;
    Check(DuplicateHandle(GetCurrentProcess(), createdMap, GetCurrentProcess(), &map,
                          0, FALSE, DUPLICATE_SAME_ACCESS) != 0,
          "producer handle duplicated for isolated corruption fixtures");
    auto* header = static_cast<framebus::HotHeader*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
    if (!map || !header) return 2;
    framebus::HotHeader fmt{};
    Check(reader.ReadFormat(fmt), "valid published format is readable");
    const framebus::HotHeader original = *header;
    header->width = 3; header->height = 3;
    Check(!reader.ReadFormat(fmt), "odd geometry rejected");
    *header = original; header->seq = 1;
    Check(!reader.ReadFormat(fmt), "in-progress format rejected");
    *header = original;
    header->width = 3448; header->height = 808; header->frameRegionBytes = framebus::kAbsMaxFrameBytes;
    Check(!reader.ReadFormat(fmt), "forged region larger than mapping rejected by existing reader");
    framebus::Reader another;
    Check(!another.TryOpen(0), "forged region rejected before a new reader caches it");
    *header = original; header->fpsNum = 0;
    Check(!reader.ReadFormat(fmt), "invalid frame rate rejected");
    *header = original; header->width = 320; header->height = 480;
    std::vector<BYTE> pixels(framebus::Yuy2Bytes(640, 240) + 16, 0x77);
    Check(reader.TryReadNewer(pixels.data(), framebus::Yuy2Bytes(640, 240), 0, 640, 240) == 0 &&
          std::all_of(pixels.begin(), pixels.end(), [](BYTE b) { return b == 0x77; }),
          "same byte count with different geometry cannot be misinterpreted");
    *header = original;
    uint32_t jpegBytes = 99;
    BYTE jpeg[16]{};
    const BYTE sourceJpeg[4]{ 1, 2, 3, 4 };
    const std::vector<BYTE> black(framebus::kMaxFrameBytes, 0);
    writer.PublishWithJpeg(black.data(), static_cast<uint32_t>(black.size()), sourceJpeg, sizeof(sourceJpeg));
    Check(reader.TryReadNewer(nullptr, framebus::kMaxFrameBytes, jpeg, sizeof(jpeg), jpegBytes, 0) != 0 &&
          jpegBytes == 0, "writer respects a zero-capacity JPEG sidecar");

    framebus::ColdMode invalid{ 0xffffffffu, 480, 30, framebus::kFmtYUY2 };
    writer.WriteColdBlock(1, framebus::kFmtYUY2, &invalid, 1);
    framebus::ColdBlock cold{};
    Check(!reader.ReadColdBlock(cold), "invalid capability geometry rejected");
    const framebus::ColdMode smallMode{ 320, 240, 15, framebus::kFmtYUY2 };
    std::atomic<bool> finished{ false };
    std::atomic<bool> observed{ false };
    std::thread publish([&] {
        writer.WriteColdBlock(11, framebus::kFmtYUY2, &smallMode, 1);
        while (!observed.load()) YieldProcessor();
        for (int i = 0; i < 20000; ++i)
        {
            writer.WriteColdBlock(11, framebus::kFmtYUY2, &smallMode, 1);
            writer.WriteColdBlock(22, framebus::kFmtYUY2, &mode, 1);
        }
        finished.store(true);
    });
    bool coherent = true;
    int snapshots = 0;
    while (!finished.load())
        if (reader.ReadColdBlock(cold))
        {
            ++snapshots;
            observed.store(true);
            coherent &= (cold.transportClass == 11 && cold.modes[0].width == 320 && cold.modes[0].fps == 15) ||
                        (cold.transportClass == 22 && cold.modes[0].width == 640 && cold.modes[0].fps == 30);
        }
    publish.join();
    Check(coherent && snapshots > 0, "concurrent capability updates yield coherent snapshots");

    HANDLE token = nullptr, restricted = nullptr;
    BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD sidBytes = sizeof(sid);
    const bool sidOk = CreateWellKnownSid(WinWorldSid, nullptr, sid, &sidBytes) != 0;
    SID_AND_ATTRIBUTES restricting{ sid, 0 };
    const bool restrictedOk = sidOk && OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token) &&
        CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1, &restricting, &restricted);
    if (restrictedOk && ImpersonateLoggedOnUser(restricted))
    {
        HANDLE writable = TestOpenMap(FILE_MAP_WRITE, FALSE, name);
        const DWORD denied = GetLastError();
        HANDLE readable = TestOpenMap(FILE_MAP_READ, FALSE, name);
        RevertToSelf();
        Check(!writable && denied == ERROR_ACCESS_DENIED, "world-restricted reader is denied section writes");
        Check(readable != nullptr, "world-restricted reader retains section read access");
        if (writable) CloseHandle(writable);
        if (readable) CloseHandle(readable);
    }
    else { printf("[SKIP] restricted token unavailable\n"); ++testSkips; }
    if (restricted) CloseHandle(restricted);
    if (token) CloseHandle(token);
    UnmapViewOfFile(header); CloseHandle(map);
    printf("framebus: %d failures, %d skips\n", testFailures, testSkips);
    return testFailures ? 1 : 0;
}
