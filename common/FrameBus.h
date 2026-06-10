#pragma once
//
// FrameBus — single-producer / multi-reader shared-memory frame transport.
//
// The host app (PS3EyeVCamHost.exe, runs elevated in the user session) is the
// writer. The media source (PS3EyeVCam.dll, loaded by the Camera Frame Server
// service as LOCAL SERVICE in session 0) is the reader. A "Global\" section is
// the only namespace visible to both, which is why the host must be elevated
// (SeCreateGlobalPrivilege).
//
// Synchronization is a seqlock: the writer makes `seq` odd while copying and
// even when done; readers copy the payload and retry if `seq` changed. No
// events or mutexes cross the boundary, so a stuck reader can never stall
// capture and N readers (FrameServer + FrameServerMonitor) work for free.
//
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <cstring>

namespace framebus {

constexpr const wchar_t* kSectionName = L"Global\\PS3EyeVCam.FrameBus";

constexpr uint32_t kMagic      = 0x50533345;  // 'PS3E'
constexpr uint32_t kVersion    = 1;
constexpr uint32_t kFormatNV12 = 1;

constexpr uint32_t kMaxWidth      = 640;      // PS3 Eye sensor maximum
constexpr uint32_t kMaxHeight     = 480;
constexpr uint32_t kDataOffset    = 4096;     // header page, then pixels
constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 3 / 2;  // NV12
constexpr uint32_t kSectionBytes  = kDataOffset + kMaxFrameBytes;

#pragma pack(push, 8)
struct Header
{
    uint32_t magic;     // kMagic once the header below is fully valid
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t fpsNum;
    uint32_t fpsDen;
    uint32_t format;    // kFormatNV12
    uint32_t pad0;
    volatile LONG64 seq;      // seqlock: odd while the writer is copying
    volatile LONG64 frameId;  // increments once per published frame
    LONG64 qpc;               // QueryPerformanceCounter at capture time
};
#pragma pack(pop)

static_assert(sizeof(Header) <= kDataOffset, "header must fit in its page");

inline uint32_t Nv12Bytes(uint32_t w, uint32_t h) { return w * h * 3 / 2; }

// ---------------------------------------------------------------- Writer ----

class Writer
{
public:
    ~Writer() { Close(); }

    // Grants: SYSTEM/Admins/LOCAL SERVICE full, Everyone + app packages read.
    // LOCAL SERVICE is what the Frame Server runs as.
    bool Create(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen)
    {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGWGX;;;WD)(A;;GRGWGX;;;AC)",
                SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
            return false;

        _map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  0, kSectionBytes, kSectionName);
        _lastError = GetLastError();
        LocalFree(sa.lpSecurityDescriptor);
        if (!_map)
            return false;

        _view = MapViewOfFile(_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
        if (!_view) { _lastError = GetLastError(); Close(); return false; }

        memset(_view, 0, kDataOffset);
        auto* h = Hdr();
        h->version = kVersion;
        h->width   = width;
        h->height  = height;
        h->fpsNum  = fpsNum;
        h->fpsDen  = fpsDen;
        h->format  = kFormatNV12;
        h->seq     = 0;
        h->frameId = 0;
        MemoryBarrier();
        h->magic = kMagic;  // readers treat the section as live from here on
        return true;
    }

    void Publish(const uint8_t* nv12, uint32_t bytes)
    {
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);            // odd: copy in progress
        memcpy(Data(), nv12, bytes);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);            // even: frame consistent
    }

    // Publishes a black frame in the current format. Used on camera-sleep
    // transitions: no stale image lingers in the (world-readable) section and
    // the next client's pre-wake frames are clean black.
    void PublishBlack()
    {
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);
        FillBlackPayload(h->width, h->height);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);
    }

    // In-place format change under the seqlock. The section is sized for the
    // largest mode and is NEVER recreated: readers (the Frame Server) keep
    // their original mapping, and a name-reuse recreate would strand them on
    // an orphaned view. frameId stays monotonic — resetting it would make
    // readers treat every future frame as stale.
    void UpdateFormat(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen)
    {
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);
        h->width  = width;
        h->height = height;
        h->fpsNum = fpsNum;
        h->fpsDen = fpsDen;
        FillBlackPayload(width, height);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);
    }

    void Close()
    {
        if (_view) { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)  { CloseHandle(_map); _map = nullptr; }
    }

    DWORD LastError() const { return _lastError; }

private:
    Header*  Hdr()  { return static_cast<Header*>(_view); }
    uint8_t* Data() { return static_cast<uint8_t*>(_view) + kDataOffset; }

    void FillBlackPayload(uint32_t w, uint32_t h)
    {
        // NV12 black: luma 0x10, chroma 0x80
        uint8_t* data = Data();
        const size_t lumaBytes = static_cast<size_t>(w) * h;
        memset(data, 0x10, lumaBytes);
        memset(data + lumaBytes, 0x80, lumaBytes / 2);
    }

    HANDLE _map = nullptr;
    void*  _view = nullptr;
    DWORD  _lastError = 0;
};

// ---------------------------------------------------------------- Reader ----

class Reader
{
public:
    ~Reader() { Close(); }

    bool TryOpen()
    {
        if (_view)
            return true;
        _map = OpenFileMappingW(FILE_MAP_READ, FALSE, kSectionName);
        if (!_map)
            return false;
        _view = MapViewOfFile(_map, FILE_MAP_READ, 0, 0, 0);
        if (!_view) { Close(); return false; }
        if (Hdr()->magic != kMagic) { Close(); return false; }
        return true;
    }

    bool IsOpen() const { return _view != nullptr; }

    void Close()
    {
        if (_view) { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)  { CloseHandle(_map); _map = nullptr; }
    }

    bool ReadFormat(Header& out)
    {
        if (!_view)
            return false;
        out = *Hdr();
        return out.magic == kMagic && out.format == kFormatNV12 &&
               out.width > 0 && out.width <= kMaxWidth &&
               out.height > 0 && out.height <= kMaxHeight &&
               out.fpsNum > 0 && out.fpsDen > 0;
    }

    // Copies the newest frame into dst iff it is newer than lastFrameId and the
    // dimensions match. Returns the consumed frameId, or 0 if nothing new.
    //
    // NOTE: this view is mapped FILE_MAP_READ, so the seqlock counters must be
    // read with plain aligned loads (atomic on x64). Interlocked* intrinsics
    // emit `lock` instructions that demand write access to the page and
    // access-violate on a read-only mapping.
    LONG64 TryReadNewer(uint8_t* dst, uint32_t dstBytes, LONG64 lastFrameId)
    {
        if (!_view)
            return 0;
        const Header* h = Hdr();
        if (h->magic != kMagic || Nv12Bytes(h->width, h->height) != dstBytes)
            return 0;

        for (int attempt = 0; attempt < 4; ++attempt)
        {
            const LONG64 seqBefore = ReadAcquire64(&h->seq);
            if (seqBefore & 1)
            {
                Sleep(0);
                continue;
            }
            const LONG64 id = ReadAcquire64(&h->frameId);
            if (id <= lastFrameId)
                return 0;

            memcpy(dst, Data(), dstBytes);

            const LONG64 seqAfter = ReadAcquire64(&h->seq);
            if (seqBefore == seqAfter)
                return id;  // copy was not torn
        }
        return 0;
    }

private:
    const Header*  Hdr()  const { return static_cast<const Header*>(_view); }
    const uint8_t* Data() const { return static_cast<const uint8_t*>(_view) + kDataOffset; }

    HANDLE _map = nullptr;
    void*  _view = nullptr;
};

} // namespace framebus
