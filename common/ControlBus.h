#pragma once
//
// ControlBus — reverse channel for camera sleep/wake coordination.
//
// The media source (DLL, inside the Frame Server as LOCAL SERVICE) stamps a
// keepalive tick whenever a client is actually consuming frames, and pulses a
// wake event so a sleeping host reacts instantly. The host streams the
// physical camera only while the tick is fresh.
//
// A timestamp (not a refcount) is used deliberately: it self-heals if the
// Frame Server crashes or leaks a stream — staleness always wins.
//
// DACL grants only SYSTEM / Administrators / LOCAL SERVICE. Unlike the frame
// section, Everyone gets nothing: a writable keepalive would let any local
// process pin the camera awake.
//
#include <windows.h>
#include <sddl.h>
#include <cstdint>

namespace controlbus {

constexpr const wchar_t* kSectionName   = L"Global\\PS3EyeVCam.Control";
constexpr const wchar_t* kWakeEventName = L"Global\\PS3EyeVCam.Wake";
constexpr const wchar_t* kSddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;LS)";

constexpr uint32_t kMagic   = 0x50334543;  // 'P3EC'
constexpr uint32_t kVersion = 1;

#pragma pack(push, 8)
struct Header
{
    uint32_t magic;
    uint32_t version;
    volatile LONG64 lastActivityTick;  // GetTickCount64() of last client activity
};
#pragma pack(pop)

// ------------------------------------------------------------------ Host ----

class Host
{
public:
    ~Host() { Close(); }

    bool Create()
    {
        SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, FALSE };
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                kSddl, SDDL_REVISION_1, &sa.lpSecurityDescriptor, nullptr))
            return false;

        _map = CreateFileMappingW(INVALID_HANDLE_VALUE, &sa, PAGE_READWRITE,
                                  0, sizeof(Header), kSectionName);
        if (_map)
            _wake = CreateEventW(&sa, FALSE /*auto-reset*/, FALSE, kWakeEventName);
        LocalFree(sa.lpSecurityDescriptor);
        if (!_map || !_wake) { Close(); return false; }

        _view = static_cast<Header*>(MapViewOfFile(_map, FILE_MAP_ALL_ACCESS, 0, 0, 0));
        if (!_view) { Close(); return false; }

        _view->version = kVersion;
        _view->lastActivityTick = 0;
        MemoryBarrier();
        _view->magic = kMagic;
        return true;
    }

    // Milliseconds since a client last consumed frames (MAXULONGLONG if never).
    ULONGLONG ActivityAgeMs() const
    {
        const LONG64 tick = ReadAcquire64(&_view->lastActivityTick);
        if (tick == 0)
            return MAXULONGLONG;
        const ULONGLONG now = GetTickCount64();
        return now > static_cast<ULONGLONG>(tick) ? now - static_cast<ULONGLONG>(tick) : 0;
    }

    HANDLE WakeEvent() const { return _wake; }
    void   ResetWake() const { ResetEvent(_wake); }

    void Close()
    {
        if (_view) { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)  { CloseHandle(_map); _map = nullptr; }
        if (_wake) { CloseHandle(_wake); _wake = nullptr; }
    }

private:
    HANDLE  _map = nullptr;
    HANDLE  _wake = nullptr;
    Header* _view = nullptr;
};

// ---------------------------------------------------------------- Pinger ----
// Used by the media source inside the Frame Server. No-ops gracefully while
// the host (and therefore the section/event) does not exist.

class Pinger
{
public:
    ~Pinger() { Close(); }

    bool IsOpen() const { return _view != nullptr; }

    bool TryOpen()
    {
        if (_view)
            return true;
        _map = OpenFileMappingW(FILE_MAP_WRITE | FILE_MAP_READ, FALSE, kSectionName);
        if (!_map)
            return false;
        _view = static_cast<Header*>(MapViewOfFile(_map, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, 0));
        if (!_view || _view->magic != kMagic)
        {
            Close();
            return false;
        }
        _wake = OpenEventW(EVENT_MODIFY_STATE, FALSE, kWakeEventName);
        return true;
    }

    // Plain aligned 64-bit store: atomic on x64. (This view is writable, but
    // stay with non-interlocked accesses on shared views as a project rule.)
    void Stamp()
    {
        if (_view)
            WriteRelease64(&_view->lastActivityTick,
                           static_cast<LONG64>(GetTickCount64()));
    }

    void SignalWake()
    {
        if (_wake)
            SetEvent(_wake);
    }

    void Close()
    {
        if (_view) { UnmapViewOfFile(_view); _view = nullptr; }
        if (_map)  { CloseHandle(_map); _map = nullptr; }
        if (_wake) { CloseHandle(_wake); _wake = nullptr; }
    }

private:
    HANDLE  _map = nullptr;
    HANDLE  _wake = nullptr;
    Header* _view = nullptr;
};

} // namespace controlbus
