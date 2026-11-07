#pragma once
//
// FrameBus — single-producer / multi-reader shared-memory frame transport.
//
// The host app (PSCam4WinTray.exe, runs elevated in the user session) is the
// writer. The media source (PSCam4Win.dll, loaded by the Camera Frame Server
// service as LOCAL SERVICE in session 0) is the reader. A "Global\" section is
// the only namespace visible to both, which is why the host must be elevated
// (SeCreateGlobalPrivilege).
//
// Synchronization is a seqlock: the writer makes `seq` odd while copying and
// even when done; readers copy the payload and retry if `seq` changed. No
// mutexes cross the boundary, so a stuck reader can never stall capture and
// N readers (FrameServer + FrameServerMonitor) work for free.
//
// A named auto-reset event ("<name>.FrameReady") is pulsed once per publish so
// readers can sleep between frames instead of polling; it carries no data and
// is purely a wakeup hint — readers always re-validate against the seqlock.
// NOTE: SetEvent on an auto-reset event releases exactly ONE waiter, so only a
// single reader per slot may WAIT on it (the Frame Server's DeliverSample);
// every other reader (preview, capability probe) must poll the seqlock.
//
// ---------------------------------------------------------------------------
// Layout (kVersion 4; one section per slot, its YUY2 region sized by the writer):
//   [   0 ..  63] HotHeader  — magic, version, payloadFormat, w, h, fps, jpegLen,
//                              frameRegionBytes, seq, frameId, qpc   (everything
//                              TryReadNewer touches; one cache line, isolated from
//                              cold capability reads)
//   [  64 ..4095] ColdBlock  — transportClass, defaultFormat, modeCount, modeTable[]
//                              (queried once at wake; never on the per-frame path)
//   [4096 ..+YUY2] YUY2 pixels                 (frameRegionBytes — never less than
//                              640x480 YUY2, at most kAbsMaxFrameBytes)
//   [ then ..+JPEG] JPEG sidecar               (jpegCapacity; filled only when an
//                              MJPEG client is active — PS3/PS4 machines never touch it.
//                              The sidecar begins at kDataOffset + frameRegionBytes.)
//
#include <windows.h>
#include <sddl.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include "IpcNames.h"
#include "Yuv.h"

namespace framebus {

constexpr uint32_t kMagic      = 0x50533345;  // 'PS3E'
constexpr uint32_t kVersion    = 4;           // bump on any layout change

// payloadFormat values. YUY2 is canonical: the host always publishes YUY2 (the
// PS3 Eye's native output, and the EyeToy's decoded output), so the no-poll
// fast path never branches on format. NV12 and beyond are reserved for future
// devices that may publish a different canonical buffer.
constexpr uint32_t kFormatYUY2 = 2;
constexpr uint32_t kFormatNV12 = 3;

// Pixel-format bits for ColdBlock formatMask / defaultFormat. A set bit means
// the device can serve that format. Values match host FormatBits
// (ICameraDevice.h) and ControlBus ConsumerFormat so a bit means the same thing
// across capability, consumer-intent, and per-frame layers.
enum FormatMaskBits : uint32_t
{
    kFmtYUY2  = 1u,
    kFmtNV12  = 2u,
    kFmtMJPEG = 4u,
};

// The YUY2 payload (YUYV 4:2:2) preserves full vertical chroma resolution, so
// the DLL's YUY2 ("original format") clients receive the frame untouched; NV12
// clients get a proper 4:2:2 -> 4:2:0 chroma downsample on the fly.
constexpr uint32_t kMaxWidth      = 640;      // PS3 Eye / EyeToy sensor maximum
constexpr uint32_t kMaxHeight     = 480;
constexpr uint32_t kDataOffset    = 4096;     // header page (HotHeader + ColdBlock), then pixels
// The 640x480 frame size: the smallest YUY2 region a section may reserve, and
// the size of device buffers genuinely bounded to <=640x480 (the PS3 capture
// buffer).
constexpr uint32_t kMaxFrameBytes = kMaxWidth * kMaxHeight * 2;  // 614400 YUY2
// The absolute ceiling any slot may reserve for its YUY2 region — the PS4
// camera's raw combined stereo frame (3448x808) — used as a sanity clamp and as
// the size of host-side staging buffers that must fit any slot.
constexpr uint32_t kAbsMaxFrameBytes = 3448u * 808u * 2u;   // 5,571,968
constexpr uint32_t kMaxJpegBytes  = 262144;   // 256 KiB JFIF sidecar (EyeToy MJPEG)
// There is no single fixed section size: Writer::Create sizes each one as
// kDataOffset + frameRegionBytes + jpegCapacity.

// ColdBlock lives at this offset, on its own cache line away from the seqlock
// fields so capability reads never false-share with per-frame publishes.
constexpr uint32_t kColdOffset    = 64;
constexpr uint32_t kMaxModes      = 32;       // >= PS3 Eye's 13 advertised modes

#pragma pack(push, 8)
struct HotHeader
{
    uint32_t magic;          // kMagic once the header below is fully valid
    uint32_t version;        // kVersion
    uint32_t payloadFormat;  // kFormatYUY2 (canonical) | kFormatNV12 (reserved)
    uint32_t width;
    uint32_t height;
    uint32_t fpsNum;
    uint32_t fpsDen;
    uint32_t jpegLen;        // JFIF byte count in the sidecar; 0 if none. <= jpeg capacity.
    uint32_t frameRegionBytes;  // bytes reserved for the YUY2 region; the JPEG sidecar
                                // (if any) begins at kDataOffset + frameRegionBytes. Set
                                // once at Create from the slot's largest mode; never changes.
    volatile LONG64 seq;     // seqlock: odd while the writer is copying
    volatile LONG64 frameId; // increments once per published frame
    LONG64 qpc;              // QueryPerformanceCounter at capture time
};

// One advertised capture mode (capability metadata, not per-frame).
struct ColdMode
{
    uint32_t width;
    uint32_t height;
    uint32_t fps;
    uint32_t formatMask;     // FormatMaskBits the device may advertise for this mode
};

// Capability block for the slot's device. Published before registration and
// updated on slot/view changes; seq protects every snapshot. transportClass and
// formatMask are stored as raw uint32_t so this header has no dependency on
// host/ICameraDevice.h.
struct ColdBlock
{
    uint32_t transportClass; // TransportClass of the slot's device
    uint32_t defaultFormat;  // FormatMaskBits value the DLL defaults to (NV12 PS3 Eye, else YUY2)
    uint32_t modeCount;      // valid entries in modes[]
    volatile LONG seq;       // capability seqlock, independent of frame publication
    ColdMode modes[kMaxModes];
};
#pragma pack(pop)

static_assert(sizeof(HotHeader) <= kColdOffset, "hot header must stay within one cache line");
static_assert(kColdOffset + sizeof(ColdBlock) <= kDataOffset, "cold block must fit in the header page");

inline bool ValidDimensions(uint32_t w, uint32_t h, uint32_t capacity)
{
    return w > 0 && h > 0 && w <= 4096 && h <= 4096 &&
           (w & 1u) == 0 && (h & 1u) == 0 &&
           static_cast<uint64_t>(w) * h * 2 <= capacity;
}

inline bool ValidRate(uint32_t num, uint32_t den)
{
    return num > 0 && den > 0 && num <= 1000000 && den <= 1000000 &&
           num >= den && static_cast<uint64_t>(num) <= static_cast<uint64_t>(den) * 1000;
}

inline uint32_t Yuy2Bytes(uint32_t w, uint32_t h) { return w * h * 2; }
inline uint32_t Nv12Bytes(uint32_t w, uint32_t h) { return w * h * 3 / 2; }

// ---------------------------------------------------------------- Writer ----

class Writer
{
public:
    ~Writer() { Close(); }

    // Grants: SYSTEM/Admins full; LOCAL SERVICE, Everyone and app packages read.
    // LOCAL SERVICE is what the Frame Server runs as. frameRegionBytes sizes the
    // YUY2 region, clamped to [kMaxFrameBytes, kAbsMaxFrameBytes]; jpegCapacity
    // is 0 for devices without an MJPEG sidecar.
    [[nodiscard]] bool Create(int cameraIndex, uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen,
                uint32_t frameRegionBytes = kMaxFrameBytes, uint32_t jpegCapacity = kMaxJpegBytes)
    {
        if (frameRegionBytes < kMaxFrameBytes) frameRegionBytes = kMaxFrameBytes;
        if (frameRegionBytes > kAbsMaxFrameBytes) frameRegionBytes = kAbsMaxFrameBytes;
        if (jpegCapacity > kMaxJpegBytes) jpegCapacity = kMaxJpegBytes;
        Close();
        if (!ValidDimensions(width, height, frameRegionBytes) || !ValidRate(fpsNum, fpsDen))
        {
            _lastError = ERROR_INVALID_PARAMETER;
            return false;
        }
        _frameRegionBytes = frameRegionBytes;
        _jpegCapacity = jpegCapacity;
        const uint32_t sectionBytes = kDataOffset + frameRegionBytes + jpegCapacity;

        wchar_t sectionName[64];
        wchar_t eventName[64];
        ipcnames::Format(sectionName, L".FrameBus", cameraIndex);
        ipcnames::Format(eventName, L".FrameReady", cameraIndex);

        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;LS)(A;;GR;;;WD)(A;;GR;;;AC)",
                SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
        {
            _lastError = GetLastError();
            return false;
        }

        _map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  0, sectionBytes, sectionName);
        _lastError = GetLastError();
        // CreateFileMapping reuses a surviving named section without applying
        // its supplied descriptor. Apply the producer policy to that object too.
        if (_map && !SetKernelObjectSecurity(_map, DACL_SECURITY_INFORMATION, sa.lpSecurityDescriptor))
        {
            _lastError = GetLastError();
            LocalFree(sa.lpSecurityDescriptor);
            Close();
            return false;
        }
        LocalFree(sa.lpSecurityDescriptor);
        if (!_map)
            return false;

        // Frame-ready wakeup event. Readers only need open/wait rights, so
        // the world principals get GR|GX (GX maps to SYNCHRONIZE for events)
        // but NOT GW: an untrusted process must not be able to SetEvent.
        SECURITY_ATTRIBUTES evSa{ sizeof(evSa), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)(A;;GRGX;;;WD)(A;;GRGX;;;AC)",
                SDDL_REVISION_1, &evSa.lpSecurityDescriptor, nullptr))
        {
            _lastError = GetLastError();
            Close();
            return false;
        }
        _frameReady = CreateEventW(&evSa, FALSE /*auto-reset*/, FALSE, eventName);
        LocalFree(evSa.lpSecurityDescriptor);
        if (!_frameReady) { _lastError = GetLastError(); Close(); return false; }

        _view = MapViewOfFile(_map, FILE_MAP_ALL_ACCESS, 0, 0, sectionBytes);
        if (!_view) { _lastError = GetLastError(); Close(); return false; }

        // Zero the whole header page (HotHeader + ColdBlock). The ColdBlock
        // stays modeCount==0 until WriteColdBlock; the host writes it before
        // registering the virtual camera, so the DLL never sees a live camera
        // without capabilities.
        memset(_view, 0, kDataOffset);
        auto* h = Hdr();
        h->version       = kVersion;
        h->payloadFormat = kFormatYUY2;
        h->width   = width;
        h->height  = height;
        h->fpsNum  = fpsNum;
        h->fpsDen  = fpsDen;
        h->jpegLen = 0;
        h->frameRegionBytes = frameRegionBytes;  // fixed for the section's lifetime
        h->seq     = 0;
        h->frameId = 0;
        MemoryBarrier();
        h->magic = kMagic;  // readers treat the section as live from here on
        return true;
    }

    // Capability changes use their own seqlock so a slot/view rescan cannot
    // tear the mode table while an existing reader builds media types.
    void WriteColdBlock(uint32_t transportClass, uint32_t defaultFormat,
                        const ColdMode* modes, uint32_t modeCount)
    {
        if (!_view || (!modes && modeCount))
            return;
        if (modeCount > kMaxModes)
            modeCount = kMaxModes;
        auto* c = Cold();
        InterlockedIncrement(&c->seq);
        c->transportClass = transportClass;
        c->defaultFormat = defaultFormat;
        c->modeCount = modeCount;
        for (uint32_t i = 0; i < modeCount; ++i)
            c->modes[i] = modes[i];
        InterlockedIncrement(&c->seq);
    }

    void Publish(const uint8_t* yuy2, uint32_t bytes)
    {
        if (bytes > _frameRegionBytes)              // never write past the YUY2 region
            bytes = _frameRegionBytes;
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);            // odd: copy in progress
        memcpy(Data(), yuy2, bytes);
        h->jpegLen = 0;                             // no JFIF sidecar this frame
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);            // even: frame consistent
        SignalFrameReady();
    }

    // Publishes a frame plus its source JFIF sidecar (EyeToy MJPEG passthrough
    // for native-MF MJPEG clients). Seqlock order:
    //   seq++ -> YUY2 -> JPEG -> jpegLen -> qpc -> frameId++ -> seq++.
    // `len` is clamped to the sidecar capacity inside the window.
    void PublishWithJpeg(const uint8_t* yuy2, uint32_t bytes,
                         const uint8_t* jpeg, uint32_t len)
    {
        if (bytes > _frameRegionBytes)              // never write past the YUY2 region
            bytes = _frameRegionBytes;
        if (len > _jpegCapacity)
            len = 0;   // an incomplete JPEG is not a valid sidecar
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);
        memcpy(Data(), yuy2, bytes);
        if (jpeg && len)
            memcpy(Jpeg(), jpeg, len);
        h->jpegLen = (jpeg ? len : 0);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);
        SignalFrameReady();
    }

    // Publishes a black frame in the current format. Used on camera-sleep
    // transitions: no stale image lingers in the (world-readable) section and
    // the next client's pre-wake frames are clean black.
    void PublishBlack()
    {
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);
        FillBlackPayload(h->width, h->height);
        h->jpegLen = 0;                             // drop any stale MJPEG sidecar
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);
        SignalFrameReady();
    }

    // In-place format change under the seqlock. The section is sized for the
    // largest mode and is NEVER recreated: readers (the Frame Server) keep
    // their original mapping, and a name-reuse recreate would strand them on
    // an orphaned view. frameId stays monotonic — resetting it would make
    // readers treat every future frame as stale.
    void UpdateFormat(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen)
    {
        if (!_view || !ValidDimensions(width, height, _frameRegionBytes) || !ValidRate(fpsNum, fpsDen))
            return;
        auto* h = Hdr();
        InterlockedIncrement64(&h->seq);
        h->width  = width;
        h->height = height;
        h->fpsNum = fpsNum;
        h->fpsDen = fpsDen;
        FillBlackPayload(width, height);
        h->jpegLen = 0;                             // drop any stale MJPEG sidecar
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        h->qpc = qpc.QuadPart;
        InterlockedIncrement64(&h->frameId);
        InterlockedIncrement64(&h->seq);
        SignalFrameReady();
    }

    void Close()
    {
        if (_view)       { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)        { CloseHandle(_map); _map = nullptr; }
        if (_frameReady) { CloseHandle(_frameReady); _frameReady = nullptr; }
    }

    DWORD LastError() const { return _lastError; }

private:
    HotHeader* Hdr()  { return static_cast<HotHeader*>(_view); }
    ColdBlock* Cold() { return reinterpret_cast<ColdBlock*>(static_cast<uint8_t*>(_view) + kColdOffset); }
    uint8_t*   Data() { return static_cast<uint8_t*>(_view) + kDataOffset; }
    uint8_t*   Jpeg() { return static_cast<uint8_t*>(_view) + kDataOffset + _frameRegionBytes; }

    void SignalFrameReady()
    {
        if (_frameReady)
            SetEvent(_frameReady);
    }

    void FillBlackPayload(uint32_t w, uint32_t h)
    {
        // Clamped to the slot's reserved YUY2 region, never to w*h alone.
        yuv::FillBlack(Data(), w, h, _frameRegionBytes);
    }

    HANDLE   _map = nullptr;
    HANDLE   _frameReady = nullptr;
    void*    _view = nullptr;
    DWORD    _lastError = 0;
    uint32_t _frameRegionBytes = kMaxFrameBytes;  // YUY2 region size; JPEG begins after it
    uint32_t _jpegCapacity = 0;
};

// ---------------------------------------------------------------- Reader ----

// Retry pacing for a reader that found a write in progress. A publish holds the
// sequence odd for the whole frame copy -- about half a millisecond for a PS4
// side-by-side frame -- which is thousands of spins. Spin briefly for the common
// short window, then yield the CPU, and give up only after a budget no publish
// comes near: failing here fails a client's camera activation outright.
class SeqRetry
{
public:
    bool Again()
    {
        if (_spins < 64) { ++_spins; YieldProcessor(); return true; }
        const ULONGLONG now = GetTickCount64();
        if (!_deadline) _deadline = now + 50;
        if (now >= _deadline) return false;
        SwitchToThread();
        return true;
    }
private:
    int       _spins = 0;
    ULONGLONG _deadline = 0;
};

class Reader
{
public:
    ~Reader() { Close(); }

    bool TryOpen(int cameraIndex)
    {
        if (_view)
            return true;
        wchar_t sectionName[64], eventName[64];
        ipcnames::Format(sectionName, L".FrameBus", cameraIndex);
        ipcnames::Format(eventName, L".FrameReady", cameraIndex);
        _map = OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName);
        if (!_map)
            return false;
        _view = MapViewOfFile(_map, FILE_MAP_READ, 0, 0, 0);
        if (!_view) { Close(); return false; }

        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(_view, &mbi, sizeof(mbi)) != sizeof(mbi) ||
            mbi.State != MEM_COMMIT || mbi.RegionSize < kDataOffset)
        { Close(); return false; }
        _mappedBytes = mbi.RegionSize;
        HotHeader fmt{};
        if (!ReadFormat(fmt)) { Close(); return false; }
        _frameRegionBytes = fmt.frameRegionBytes;
        const size_t spare = _mappedBytes - kDataOffset - _frameRegionBytes;
        _sidecarBytes = static_cast<uint32_t>(spare > kMaxJpegBytes ? kMaxJpegBytes : spare);
        _frameReady = OpenEventW(SYNCHRONIZE, FALSE, eventName);
        if (!_frameReady) { Close(); return false; }
        return true;
    }

    bool IsOpen() const { return _view != nullptr; }

    void Close()
    {
        if (_view) { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map) { CloseHandle(_map); _map = nullptr; }
        if (_frameReady) { CloseHandle(_frameReady); _frameReady = nullptr; }
        _mappedBytes = 0;
        _frameRegionBytes = 0;
        _sidecarBytes = 0;
    }

    bool ReadFormat(HotHeader& out)
    {
        if (!_view)
            return false;
        for (SeqRetry retry; retry.Again(); )
        {
            const LONG64 seq = ReadAcquire64(&Hdr()->seq);
            if (seq & 1) continue;
            HotHeader snapshot = *Hdr();
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq != ReadNoFence64(&Hdr()->seq))
                continue;                   // torn by a publish: read again
            if (!ValidHeader(snapshot))
                return false;               // coherent but invalid: not transient
            out = snapshot;
            return true;
        }
        return false;
    }

    bool ReadColdBlock(ColdBlock& out)
    {
        HotHeader fmt{};
        if (!ReadFormat(fmt))
            return false;
        for (SeqRetry retry; retry.Again(); )
        {
            const LONG seq = ReadAcquire(&Cold()->seq);
            if (seq & 1) continue;
            ColdBlock snapshot = *Cold();
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq != ReadNoFence(&Cold()->seq))
                continue;
            if (!snapshot.modeCount || snapshot.modeCount > kMaxModes ||
                (snapshot.defaultFormat != kFmtYUY2 && snapshot.defaultFormat != kFmtNV12 &&
                 snapshot.defaultFormat != kFmtMJPEG))
                return false;
            for (uint32_t i = 0; i < snapshot.modeCount; ++i)
            {
                const auto& m = snapshot.modes[i];
                if (!ValidDimensions(m.width, m.height, fmt.frameRegionBytes) ||
                    !ValidRate(m.fps, 1) || !m.formatMask ||
                    (m.formatMask & ~(kFmtYUY2 | kFmtNV12 | kFmtMJPEG)))
                    return false;
            }
            out = snapshot;
            return true;
        }
        return false;
    }

    void WaitFrame(DWORD timeoutMs) const
    {
        if (_frameReady)
            WaitForSingleObject(_frameReady, timeoutMs);
    }

    // Expected geometry is checked inside the payload sequence window.
    // A backwards frameId is accepted to recover a restarted producer.
    LONG64 TryReadNewer(uint8_t* dst, uint32_t dstBytes, LONG64 lastFrameId,
                        uint32_t expectedWidth = 0, uint32_t expectedHeight = 0)
    {
        uint32_t unused = 0;
        return dst ? TryReadNewer(dst, dstBytes, nullptr, 0, unused, lastFrameId,
                                  expectedWidth, expectedHeight) : 0;
    }

    LONG64 TryReadNewer(uint8_t* dst, uint32_t dstBytes,
                        uint8_t* jpegDst, uint32_t jpegCap, uint32_t& outJpegLen,
                        LONG64 lastFrameId, uint32_t expectedWidth = 0,
                        uint32_t expectedHeight = 0)
    {
        outJpegLen = 0;
        if (!_view)
            return 0;
        for (int attempt = 0; attempt < 64; ++attempt)
        {
            const LONG64 seq = ReadAcquire64(&Hdr()->seq);
            if (seq & 1) { YieldProcessor(); continue; }
            const HotHeader h = *Hdr();
            if (!ValidHeader(h) || Yuy2Bytes(h.width, h.height) != dstBytes ||
                (expectedWidth && h.width != expectedWidth) ||
                (expectedHeight && h.height != expectedHeight) ||
                h.frameId <= 0 || h.frameId == lastFrameId)
                continue;
            if (jpegDst && (h.jpegLen > _sidecarBytes || h.jpegLen > jpegCap))
                return 0;
            if (dst)
                memcpy(dst, Data(), dstBytes);
            if (jpegDst && h.jpegLen)
                memcpy(jpegDst, Jpeg(), h.jpegLen);
            std::atomic_thread_fence(std::memory_order_acquire);
            if (seq == ReadNoFence64(&Hdr()->seq))
            {
                outJpegLen = jpegDst ? h.jpegLen : 0;
                return h.frameId;
            }
        }
        return 0;
    }

private:
    bool ValidHeader(const HotHeader& h) const
    {
        return h.magic == kMagic && h.version == kVersion && h.payloadFormat == kFormatYUY2 &&
               h.frameRegionBytes > 0 && h.frameRegionBytes <= kAbsMaxFrameBytes &&
               (!_frameRegionBytes || h.frameRegionBytes == _frameRegionBytes) &&
               static_cast<uint64_t>(kDataOffset) + h.frameRegionBytes <= _mappedBytes &&
               ValidDimensions(h.width, h.height, h.frameRegionBytes) &&
               ValidRate(h.fpsNum, h.fpsDen);
    }
    const HotHeader* Hdr() const { return static_cast<const HotHeader*>(_view); }
    const ColdBlock* Cold() const { return reinterpret_cast<const ColdBlock*>(static_cast<const uint8_t*>(_view) + kColdOffset); }
    const uint8_t* Data() const { return static_cast<const uint8_t*>(_view) + kDataOffset; }
    const uint8_t* Jpeg() const { return Data() + _frameRegionBytes; }

    HANDLE _map = nullptr;
    HANDLE _frameReady = nullptr;
    void* _view = nullptr;
    size_t _mappedBytes = 0;
    uint32_t _frameRegionBytes = 0;
    uint32_t _sidecarBytes = 0;
};

} // namespace framebus
