#pragma once
//
// AudioStatusBus — one small shared-memory block describing the PS4 microphone
// and where it is being rendered.
//
// Exposes in-process microphone health and the renderer's ring fill and drift
// correction to external readers without putting audio samples on the bus.
//
// Each microphone-owning slot publishes its own block (Name(slot)); a split
// pair's Right half reads its home slot's. Diagnostic tools that do not know
// the slot use Reader::OpenAny(), which also accepts the unsuffixed block an
// older tray publishes.
//
// Published at 10 Hz from the capture thread while the camera streams, read by
// anyone. There is no
// lock: every field is either atomic-width or only meaningful alongside `seq`,
// which is bumped after the payload is written. A reader that wants a coherent
// snapshot reads seq, copies, reads seq again, and retries if it moved —
// the same seqlock discipline FrameBus uses.
//
#include <windows.h>
#include <sddl.h>
#include <cstddef>   // offsetof, for the payload-only publish below
#include <cstdint>
#include <string>
#include "VCamGuids.h"   // kVCamCount

namespace audiostatus {

constexpr uint32_t kMagic   = 0x53344153;   // 'S4AS'
constexpr uint32_t kVersion = 1;
constexpr wchar_t  kName[]  = L"Global\\PSCam4Win.AudioStatus";
inline std::wstring Name(int slot)
{ return slot < 0 ? kName : std::wstring(kName) + L"." + std::to_wstring(slot); }

struct Block
{
    uint32_t magic;
    uint32_t version;
    volatile uint64_t seq;          // odd while being written (seqlock)

    // ---- capture side ----
    int32_t  slot;                  // which camera owns the array; -1 = none
    uint32_t channels;              // 0 when no microphone is live
    uint32_t nominalRate;           // what the DSP resamples TO (48,000)
    uint32_t measuredRateMilliHz;   // what it REALLY delivers (~48,023,000)
    uint32_t dropouts;              // concealed gaps this session
    uint32_t levels[4];             // per-channel RMS x 10000

    // ---- render side ----
    uint32_t renderActive;
    uint32_t renderFillMs;          // ring occupancy: the drift loop's input
    int32_t  renderPpm;             // correction it settled on
    uint32_t renderUnderruns;       // ring ran dry -> audible gap
    uint32_t renderOverruns;        // ring filled -> audio discarded
    uint32_t endpointRate;
    uint32_t endpointChannels;
    wchar_t  endpointName[128];

    // GetTickCount64() at the last Publish. WITHOUT THIS THE BLOCK LIES: it is
    // written only while the camera streams, so when the camera sleeps the last
    // snapshot simply stays there, and "4 ch, rendering at 48000 Hz" reads
    // identically whether that is happening now or stopped ten minutes ago.
    uint64_t publishTick;
};

// How old a snapshot may be and still describe the present. The writer publishes
// every 100 ms while streaming, so anything past a second means the pump has
// stopped -- the camera slept, or the tray died.
constexpr uint64_t kFreshMs = 1500;

// Writer: the tray. Creates the block and publishes into it.
class Writer
{
public:
    ~Writer() { Close(); }

    bool Create(int slot = -1)
    {
        if (_view) return true;
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;LS)(A;;GR;;;WD)(A;;GR;;;AC)",
                SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr)) return false;
        _map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  0, sizeof(Block), Name(slot).c_str());
        // CreateFileMapping reuses a surviving named section without applying
        // the supplied descriptor, so apply it explicitly.
        const bool secured = _map && SetKernelObjectSecurity(_map, DACL_SECURITY_INFORMATION, sa.lpSecurityDescriptor);
        LocalFree(sa.lpSecurityDescriptor);
        if (!secured) { Close(); return false; }
        _view = (Block*)MapViewOfFile(_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Block));
        if (!_view) { CloseHandle(_map); _map = nullptr; return false; }
        _view->magic = kMagic;
        _view->version = kVersion;
        _view->slot = -1;
        return true;
    }

    void Close()
    {
        if (_view) { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)  { CloseHandle(_map); _map = nullptr; }
    }

    bool Valid() const { return _view != nullptr; }

    // Publish a snapshot. Bumps seq to odd, writes, bumps to even — so a reader
    // can tell it caught a torn write and retry rather than believe it.
    void Publish(const Block& b)
    {
        if (!_view) return;
        const uint64_t odd = _view->seq | 1;      // mark: write in progress
        _view->seq = odd;
        MemoryBarrier();

        // Copy the PAYLOAD ONLY, never `seq`. A whole-struct memcpy would write
        // the caller's zero-initialised seq over the live one, so for the length
        // of the copy the block would read EVEN -- the value that means
        // "consistent" -- and a reader could take a torn snapshot and believe
        // it. Everything from `slot` onward is payload.
        memcpy((void*)&_view->slot, &b.slot, sizeof(Block) - offsetof(Block, slot));
        _view->magic = kMagic;
        _view->version = kVersion;
        _view->publishTick = GetTickCount64();   // stamped here, never by callers

        MemoryBarrier();
        _view->seq = odd + 1;                     // even again: consistent
    }

private:
    HANDLE _map = nullptr;
    Block* _view = nullptr;
};

// Reader: the Settings window and any diagnostic tool.
class Reader
{
public:
    ~Reader() { Close(); }

    bool Open(int slot = -1)
    {
        if (_view && _slot == slot) return true;
        Close(); _slot = slot;
        _map = OpenFileMappingW(FILE_MAP_READ, FALSE, Name(slot).c_str());
        if (!_map) return false;
        _view = (const Block*)MapViewOfFile(_map, FILE_MAP_READ, 0, 0, sizeof(Block));
        if (!_view) { CloseHandle(_map); _map = nullptr; return false; }
        return true;
    }

    // Diagnostic selection: prefer a fresh per-slot writer, while permitting
    // the unsuffixed mapping produced by an older installed tray.
    bool OpenAny()
    {
        Block b{};
        int fallback = -1;
        for (int slot = 0; slot < kVCamCount; ++slot)
            if (Open(slot) && Read(b))
            {
                if (Fresh(b) && b.channels) return true;
                if (fallback < 0) fallback = slot;
            }
        if (Open() && Read(b)) return true;
        return fallback >= 0 && Open(fallback);
    }

    void Close()
    {
        if (_view) { UnmapViewOfFile((LPCVOID)_view); _view = nullptr; }
        if (_map)  { CloseHandle(_map); _map = nullptr; }
    }

    // Coherent snapshot, or false if the block is absent/invalid or the writer
    // kept moving under us.
    bool Read(Block& out) const
    {
        if (!_view) return false;
        for (int attempt = 0; attempt < 8; ++attempt)
        {
            const uint64_t a = _view->seq;
            if (a & 1) { YieldProcessor(); continue; }         // mid-write
            MemoryBarrier();
            memcpy(&out, (const void*)_view, sizeof(Block));
            MemoryBarrier();
            if (_view->seq == a)
                return out.magic == kMagic && out.version == kVersion;
        }
        return false;
    }

    // True when the snapshot describes the present rather than the last moment
    // the camera was awake. Every consumer that says "is it working NOW" must
    // ask this -- Read() alone succeeds happily on a stale block.
    static bool Fresh(const Block& b)
    {
        const uint64_t now = GetTickCount64();
        return b.publishTick != 0 && now >= b.publishTick &&
               (now - b.publishTick) <= kFreshMs;
    }

private:
    HANDLE _map = nullptr;
    const Block* _view = nullptr;
    int _slot = -1;
};

} // namespace audiostatus
