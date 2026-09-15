#pragma once
//
// CaptureController — the camera thread, one per slot. Owns everything hardware-
// and MF-related: the slot's ICameraDevice (whichever transport the
// DeviceRegistry hands it), the FrameBus writer, the ControlBus host, and the
// IMFVirtualCamera registration. It never switches on a product enum — device
// differences live behind ICameraDevice and its static DeviceProfile. Runs the
// sleep/wake state machine:
//
//   ASLEEP    camera fully released (USB closed, LED off, 0% CPU); waiting on
//             the ControlBus wake event the DLL pulses when a client streams
//   WAKING    re-enumerate + init + start (~0.7s, clients see black frames)
//   STREAMING AcquireFrame (the device does its own debayer / JPEG decode /
//             band copy) -> FrameBus publish; back to ASLEEP when the DLL
//             keepalive goes stale (no client for idleTimeoutMs)
//
// The IMFVirtualCamera registration is dynamic: it exists only while a physical
// camera occupies this slot, and is advertised under that device's own name, so
// apps see exactly the cameras that are plugged in. Arrival/removal is signaled
// by the tray window's
// device-interface notification through NotifyDeviceChange(), with a slow
// fallback poll while the slot is empty.
//
// The UI thread NEVER touches the camera: it calls UpdateSettings() with a
// full snapshot; this thread drains it at safe points. Mode changes apply
// immediately while asleep and are deferred to the next sleep transition
// while streaming (changing the bus format under a live client would freeze
// that client's picture).
//
#include <windows.h>
#include <atomic>
#include <string>
#include "../common/Settings.h"

class CaptureController
{
public:
    enum class State : int
    {
        Starting = 0,
        Asleep,         // virtual camera registered, physical camera off
        Waking,
        Streaming,
        CameraMissing,  // PS3 Eye not found / unplugged (retrying)
        VCamFailed,     // MFCreateVirtualCamera/Start failing (retrying)
        Ps4NeedsReplug, // PS4 present but its firmware session is wedged (replug)
        Fatal,          // unrecoverable (e.g. shared memory creation failed)
    };

    // Begins the camera thread. State changes are announced with
    // PostMessage(notifyWnd, notifyMsg, (WPARAM)State, (LPARAM)cameraIndex).
    bool Start(int cameraIndex, HWND notifyWnd, UINT notifyMsg);
    void Stop();  // signals and joins the thread; safe to call twice

    // Full-snapshot settings handoff from the UI thread.
    void UpdateSettings(const Settings& s);

    // Pokes the camera thread to re-evaluate slot occupancy (called by the
    // tray window on WM_DEVICECHANGE for the PS3 Eye interface class).
    void NotifyDeviceChange();

    State    GetState() const { return _state.load(std::memory_order_relaxed); }
    bool     HasPendingModeChange() const { return _pendingMode.load(std::memory_order_relaxed); }
    // Capture rate over the last measurement window, x10 (594 == 59.4 fps).
    uint32_t MeasuredFpsX10() const { return _fpsX10.load(std::memory_order_relaxed); }

    // ---- microphone (PS4 only; see ICameraDevice::AudioChannels) ----------
    // The camera thread owns the device, so the UI cannot call it directly.
    // Levels are published into atomics once per captured frame, and recording
    // is a flag the thread observes — no locks on the UI side.
    static const uint32_t kMicMaxChannels = 4;

    // 0 when the selected camera has no in-band microphone.
    uint32_t MicChannels() const { return _micChannels.load(std::memory_order_relaxed); }
    // Per-channel RMS 0..1 from the most recent captured frame.
    void     MicLevels(float* out, uint32_t count) const;
    bool     MicRecording() const { return _micRecording.load(std::memory_order_relaxed); }
    uint32_t MicRecordedSeconds() const { return _micRecSecs.load(std::memory_order_relaxed); }
    uint32_t MicDropouts() const { return _micDropouts.load(std::memory_order_relaxed); }
    // Begin writing a 4-channel WAV; returns the chosen path (empty on failure).
    // Path of the most recent recording, empty until one is started. Written by
    // MicStartRecording and read by the dialog, both on the UI thread.
    const std::wstring& MicRecordingPath() const { return _micRecPath; }
    std::wstring MicStartRecording();
    void         MicStopRecording() { _micRecording.store(false, std::memory_order_release); }

    Settings ActiveSettings() const;

    // In-process request from the Settings dialog preview to keep the camera
    // streaming while the dialog is open, independent of the ControlBus
    // keepalive (which only fires when an external app consumes frames).
    // Cleared on dialog close. Does not touch the ControlBus protocol.
    // Turning hold on pokes the camera thread so an Asleep controller
    // re-evaluates clientFresh() immediately instead of waiting for the next
    // external wake.
    void SetPreviewHold(bool held);

    // True when the preview dialog is the only thing keeping this camera
    // streaming (preview-hold on, no external client has a fresh keepalive).
    // Used to suppress the "mode change queued" balloon — the mode applies
    // immediately when the preview is the only consumer.
    bool IsPreviewOnly() const
    {
        return _previewHold.load(std::memory_order_relaxed) &&
               !_externalClient.load(std::memory_order_relaxed);
    }

    // An app outside this process is consuming frames right now.
    bool HasExternalClient() const
    {
        return _externalClient.load(std::memory_order_relaxed);
    }

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void Run();
    void SetState(State s);

    int    _cameraIndex = 0;
    HWND   _notifyWnd = nullptr;
    UINT   _notifyMsg = 0;
    HANDLE _thread = nullptr;
    HANDLE _stopEvent = nullptr;   // manual-reset
    HANDLE _cmdEvent = nullptr;    // auto-reset, pulsed by UpdateSettings
    HANDLE _rescanEvent = nullptr; // auto-reset, pulsed by NotifyDeviceChange

    mutable SRWLOCK _settingsLock = SRWLOCK_INIT;
    Settings _desired;             // written by UI thread
    Settings _active;              // owned by camera thread, mirrored for UI reads

    std::atomic<State>    _state{ State::Starting };
    std::atomic<bool>     _settingsDirty{ false };
    std::atomic<bool>     _pendingMode{ false };
    std::atomic<bool>     _previewHold{ false };
    // Mic-only wake: set while this slot's camera HAS a microphone array and the
    // user has chosen an output to render it to. Keeps the camera streaming with
    // no video client at all, because the PS4's array rides inside the video
    // stream and would otherwise go silent ~3 s after the last app closed.
    // Decided in two places (the capture pump, and the Asleep pass which is the
    // only one that runs when there is nothing to pump) — see clientFresh().
    std::atomic<bool>     _micHold{ false };
    std::atomic<bool>     _externalClient{ false };  // fresh ControlBus keepalive
    std::atomic<uint32_t> _fpsX10{ 0 };

    // microphone state (see the accessors above)
    std::atomic<uint32_t> _micChannels{ 0 };
    std::atomic<uint32_t> _micLevel[kMicMaxChannels] = {};   // RMS * 10000
    std::atomic<bool>     _micRecording{ false };
    std::atomic<uint32_t> _micRecSecs{ 0 };
    std::atomic<uint32_t> _micDropouts{ 0 };
    std::wstring          _micRecPath;                       // set before the flag

};
