#pragma once
//
// AudioStatusBus — one small shared-memory block describing the PS4 microphone
// and where it is being rendered.
//
// Exposes in-process microphone health and the renderer's ring fill and drift
// correction to external readers without putting audio samples on the bus.
//
// Deliberately ONE block, not one per slot: there is a single microphone array
// and a single renderer, so a per-slot name would imply a fan-out that does not
// exist. `slot` says which camera it belongs to.
//
// Published at 10 Hz from the capture thread, read by anyone. There is no
// lock: every field is either atomic-width or only meaningful alongside `seq`,
// which is bumped after the payload is written. A reader that wants a coherent
// snapshot reads seq, copies, reads seq again, and retries if it moved —
// the same seqlock discipline FrameBus uses.
//
#include <windows.h>
#include <cstddef>   // offsetof, for the payload-only publish below
#include <cstdint>

namespace audiostatus {

constexpr uint32_t kMagic   = 0x53344153;   // 'S4AS'
constexpr uint32_t kVersion = 1;
constexpr wchar_t  kName[]  = L"Global\\PSCam4Win.AudioStatus";

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
    // written once per captured frame, so when the camera sleeps the last
    // snapshot simply stays there, and "4 ch, rendering at 48000 Hz" reads
    // identically whether that is happening now or stopped ten minutes ago.
    // That cost a real debugging session -- a keep-awake test looked broken
    // because the stale block still said channels=4 after the camera slept.
    uint64_t publishTick;
};

// How old a snapshot may be and still describe the present. The writer publishes
// per video frame (60/s at the usual mode, 15/s at the slowest), so anything
// past a second means the pump has stopped -- the camera slept, or the tray died.
constexpr uint64_t kFreshMs = 1500;

// Writer: the tray. Creates the block and publishes into it.
class Writer
{
public:
    ~Writer() { Close(); }

    bool Create()
    {
        if (_view) return true;
        _map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                                  0, sizeof(Block), kName);
        if (!_map) return false;
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

        // Copy the PAYLOAD ONLY, never `seq`. A whole-struct memcpy wrote the
        // caller's zero-initialised seq over the live one, so for the length of
        // the copy the block read EVEN -- the value that means "consistent" --
        // and a reader could take a torn snapshot and believe it. The seqlock
        // was silently not a seqlock. Everything from `slot` onward is payload.
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

// Reader: the suite, mic_route, anything diagnostic.
class Reader
{
public:
    ~Reader() { Close(); }

    bool Open()
    {
        if (_view) return true;
        _map = OpenFileMappingW(FILE_MAP_READ, FALSE, kName);
        if (!_map) return false;
        _view = (const Block*)MapViewOfFile(_map, FILE_MAP_READ, 0, 0, sizeof(Block));
        if (!_view) { CloseHandle(_map); _map = nullptr; return false; }
        return true;
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
            if (a & 1) { Sleep(0); continue; }         // mid-write
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
};

} // namespace audiostatus
