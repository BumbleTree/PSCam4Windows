#include "CaptureController.h"
#include "WavWriter.h"
#include "AudioRender.h"
#include "../common/AudioStatusBus.h"

#include <shlobj.h>   // SHGetFolderPathW / CSIDL_MYVIDEO for the mic recording path

#include <mfapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <memory>

#include "HostLog.h"
#include "ICameraDevice.h"
#include "DeviceRegistry.h"
#include "DeviceProfiles.h"
#include "mfvirtualcamera_min.h"
#include "../common/FrameBus.h"
#include "../common/ControlBus.h"
#include "../common/VCamGuids.h"

using Microsoft::WRL::ComPtr;

namespace
{

// MFCreateVirtualCamera lives in mfsensorgroup.dll (Windows 11 22000+).
// Resolved once for the whole process; the module is never freed (its
// lifetime is the process's). Thread-safe via C++11 magic-static init.
PFN_MFCreateVirtualCamera GetMFCreateVirtualCamera()
{
    static PFN_MFCreateVirtualCamera fn = []() -> PFN_MFCreateVirtualCamera {
        HMODULE module = LoadLibraryW(L"mfsensorgroup.dll");
        return module ? reinterpret_cast<PFN_MFCreateVirtualCamera>(
                            GetProcAddress(module, "MFCreateVirtualCamera"))
                      : nullptr;
    }();
    return fn;
}

} // namespace

// ---------------------------------------------------------------------------

bool CaptureController::Start(int cameraIndex, HWND notifyWnd, UINT notifyMsg)
{
    _cameraIndex = cameraIndex;
    _notifyWnd = notifyWnd;
    _notifyMsg = notifyMsg;
    {
        AcquireSRWLockExclusive(&_settingsLock);
        _desired = settings::Load(_cameraIndex);
        // Device-aware initial mode: if a recognised camera occupies this slot
        // and the persisted mode isn't one it can serve, fall back to that
        // device's default mode (e.g. an EyeToy slot persisted with the PS3's
        // 640x480@60 default). No-op for the PS3 Eye (its default mode is one of
        // its own modes) and for empty slots (no profile), so the PS3 path and
        // slot↔mode mapping are unchanged.
        if (const DeviceProfile* prof = deviceregistry::ProfileForSlot(_cameraIndex))
        {
            if (!ProfileHasMode(*prof, _desired.width, _desired.height, _desired.fps))
            {
                _desired.width  = prof->defaultMode.width;
                _desired.height = prof->defaultMode.height;
                _desired.fps    = prof->defaultMode.fps;
            }
        }
        _active = _desired;
        ReleaseSRWLockExclusive(&_settingsLock);
    }
    _stopEvent   = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    _cmdEvent    = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    _rescanEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!_stopEvent || !_cmdEvent || !_rescanEvent)
        return false;
    _thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    return _thread != nullptr;
}

void CaptureController::Stop()
{
    if (_stopEvent)
        SetEvent(_stopEvent);
    bool joined = true;
    if (_thread)
    {
        joined = (WaitForSingleObject(_thread, 15000) == WAIT_OBJECT_0);
        if (!joined)
            HostLog(L"camera %d: thread did not exit within 15s -- abandoning it", _cameraIndex);
        CloseHandle(_thread);
        _thread = nullptr;
    }

    // Leak the events if the join failed: an abandoned thread is still waiting on
    // them and still owns a FrameBus writer on its stack, so closing them risks a
    // use-after-free on the recycled handles. The process is exiting anyway.
    if (!joined)
    {
        _stopEvent   = nullptr;
        _cmdEvent    = nullptr;
        _rescanEvent = nullptr;
        return;
    }

    if (_stopEvent)   { CloseHandle(_stopEvent);   _stopEvent = nullptr; }
    if (_cmdEvent)    { CloseHandle(_cmdEvent);    _cmdEvent = nullptr; }
    if (_rescanEvent) { CloseHandle(_rescanEvent); _rescanEvent = nullptr; }
}

void CaptureController::UpdateSettings(const Settings& s)
{
    AcquireSRWLockExclusive(&_settingsLock);
    _desired = s;
    ReleaseSRWLockExclusive(&_settingsLock);
    _settingsDirty.store(true, std::memory_order_release);
    if (_cmdEvent)
        SetEvent(_cmdEvent);
}

void CaptureController::NotifyDeviceChange()
{
    if (_rescanEvent)
        SetEvent(_rescanEvent);
}

Settings CaptureController::ActiveSettings() const
{
    AcquireSRWLockShared(&_settingsLock);
    Settings s = _active;
    ReleaseSRWLockShared(&_settingsLock);
    return s;
}

void CaptureController::SetPreviewHold(bool held)
{
    _previewHold.store(held, std::memory_order_relaxed);
    // Turning hold on must wake an Asleep controller so it re-evaluates
    // clientFresh() immediately; otherwise it stays parked in
    // WaitForMultipleObjects until some external event arrives. Turning hold
    // off needs no nudge: the next stale-keepalive check puts the camera back
    // to sleep on its own.
    if (held && _cmdEvent)
        SetEvent(_cmdEvent);
}

void CaptureController::SetState(State s)
{
    if (_state.exchange(s, std::memory_order_relaxed) != s && _notifyWnd)
        PostMessage(_notifyWnd, _notifyMsg, static_cast<WPARAM>(static_cast<int>(s)), static_cast<LPARAM>(_cameraIndex));
}

DWORD WINAPI CaptureController::ThreadProc(LPVOID self)
{
    static_cast<CaptureController*>(self)->Run();
    return 0;
}

void CaptureController::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION);

    Settings active = ActiveSettings();

    framebus::Writer bus;
    // A slot's occupant can change at runtime (hot-plug), but the FrameBus section
    // is sized ONCE here and is NEVER recreated (recreating would strand the DLL
    // reader on an orphaned mapping). So it must fit the largest frame ANY supported
    // device could EVER publish into this slot — not merely whatever (if anything)
    // occupies it at startup. Any of the 8 slots can host a PS4, whose Side-by-side
    // view is 2560x800 YUY2 (4 MB); kAbsMaxFrameBytes is the documented absolute
    // ceiling (and already sizes the host-side staging buffers). PS3 Eye / EyeToy
    // (<=640x480) simply under-fill the over-provisioned section.
    //
    // Sizing from the startup occupant instead is unsafe: a PS4 plugged into a slot
    // that was empty (the normal "replug after restart" flow) or that held a
    // PS3/EyeToy at launch would publish a 2-4 MB frame into the 614 KB legacy
    // section and overrun it (host crash). PublishBlack/Publish are also clamped to
    // the section size in FrameBus.h as defence-in-depth.
    if (!bus.Create(_cameraIndex, active.width, active.height, active.fps, 1,
                    framebus::kAbsMaxFrameBytes))
    {
        HostLog(L"FATAL: FrameBus creation failed (win32=%lu) -- not elevated?", bus.LastError());
        SetState(State::Fatal);
        MFShutdown();
        CoUninitialize();
        return;
    }
    bus.PublishBlack();

    // Publish the slot's device capabilities (modes / formats / default) before
    // any virtual-camera registration, so the DLL always has a capability block
    // to build its media types from.
    deviceregistry::PublishColdBlock(bus, _cameraIndex);

    controlbus::Host control;
    if (!control.Create(_cameraIndex))
    {
        HostLog(L"FATAL: ControlBus creation failed (win32=%lu)", GetLastError());
        SetState(State::Fatal);
        MFShutdown();
        CoUninitialize();
        return;
    }

    // ---- virtual camera registration (dynamic, slot-occupancy driven) -----
    PFN_MFCreateVirtualCamera createVCam = GetMFCreateVirtualCamera();

    ComPtr<IMFVirtualCamera> vcam;
    bool vcamLive = false;
    ULONGLONG vcamRetryDue = 0;  // GetTickCount64 deadline for next attempt
    int vcamAttempts = 0;

    auto tryRegisterVCam = [&]() {
        if (vcamLive || !createVCam || GetTickCount64() < vcamRetryDue)
            return;
        ++vcamAttempts;
        // The virtual camera advertises the name of the device that currently
        // occupies the slot ("PS3 Eye" / "PS2 EyeToy"), numbered only when more
        // than one camera of that type is present, so a lone EyeToy is just
        // "PS2 EyeToy" rather than "PS2 EyeToy #7".
        wchar_t friendlyName[64];
        deviceregistry::SlotDisplayName(_cameraIndex, friendlyName, 64);
        HRESULT hr = createVCam(MFVirtualCameraType_SoftwareCameraSource,
                                MFVirtualCameraLifetime_Session,
                                MFVirtualCameraAccess_CurrentUser,
                                friendlyName, kVCamClsidStrings[_cameraIndex], nullptr, 0, &vcam);
        if (SUCCEEDED(hr))
            hr = vcam->Start(nullptr);
        if (SUCCEEDED(hr))
        {
            vcamLive = true;
            vcamAttempts = 0;
            HostLog(L"camera %d: virtual camera registered and started", _cameraIndex);
        }
        else
        {
            vcam.Reset();
            // Right after logon MF may not be ready yet: 5 quick attempts at
            // 2s, then every 30s forever.
            vcamRetryDue = GetTickCount64() + (vcamAttempts < 5 ? 2000 : 30000);
            HostLog(L"camera %d: virtual camera registration failed 0x%08X (attempt %d)",
                    _cameraIndex, hr, vcamAttempts);
            SetState(State::VCamFailed);
        }
    };

    auto unregisterVCam = [&]() {
        if (!vcamLive && !vcam)
            return;
        if (vcam)
        {
            vcam->Stop();
            vcam->Remove();
            vcam->Shutdown();
            vcam.Reset();
        }
        vcamLive = false;
        vcamAttempts = 0;
        vcamRetryDue = 0;
        HostLog(L"camera %d: virtual camera unregistered", _cameraIndex);
    };

    if (!createVCam)
    {
        HostLog(L"FATAL: MFCreateVirtualCamera unavailable -- Windows 11 22000+ required");
        SetState(State::Fatal);
    }

    // ---- camera state ------------------------------------------------------
    // The active device for this slot (PS3 Eye today). Built by the registry on
    // wake, destroyed on sleep. AcquireFrame returns device-owned YUY2; the
    // fused Bayer->YUY2 debayer lives inside the device, no RGB intermediate.
    std::unique_ptr<ICameraDevice> device;

    // True while any of: (a) an external app is consuming frames (ControlBus
    // keepalive fresh), (b) the Settings dialog preview is open for this camera,
    // or (c) the microphone is being routed to an output device. None touch the
    // wire protocol — (b) and (c) are in-process tray signals only.
    //
    // (c) is the mic-only wake. The PS4's array rides inside the video stream,
    // so audio exists only while video is streaming — and without this the
    // microphone would die about three seconds after the last camera app closed,
    // which is not a microphone. It is a keep-awake, and it is the only one that
    // should exist: a keep-awake that merely holds the stream open to dodge a
    // device bug is a workaround and does not belong here.
    //
    // It costs USB bandwidth for as long as it is on, which is why it is off
    // unless the user has actually chosen an output device.
    auto clientFresh = [&]() {
        const bool ext = control.ActivityAgeMs() < active.idleTimeoutMs;
        _externalClient.store(ext, std::memory_order_relaxed);
        return ext || _previewHold.load(std::memory_order_relaxed)
                   || _micHold.load(std::memory_order_relaxed);
    };

    // Re-sync the slot's advertised capabilities (and the active mode) with the
    // device that currently occupies it. Cold path: only ever called while
    // Asleep (the camera is released), so rewriting the bus format and active
    // mode here cannot disturb a live stream. Covers a camera hot-plugged into a
    // slot that was empty — or held a different device — when Run() first
    // published the ColdBlock, including the logon race where the USB stack had
    // not finished enumerating the camera by the time Run() started.
    auto readvertiseSlot = [&](const DeviceProfile* prof) {
        // If the active/persisted mode isn't one this device can serve (e.g. an
        // EyeToy landing in a slot that defaulted to the PS3's 640x480@60), fall
        // back to the device's default so Init() gets a valid mode and the bus
        // header advertises the real geometry.
        if (!ProfileHasMode(*prof, active.width, active.height, active.fps))
        {
            active.width  = prof->defaultMode.width;
            active.height = prof->defaultMode.height;
            active.fps    = prof->defaultMode.fps;
            bus.UpdateFormat(active.width, active.height, active.fps, 1);
            _pendingMode.store(false, std::memory_order_relaxed);
            AcquireSRWLockExclusive(&_settingsLock);
            _active = active;
            ReleaseSRWLockExclusive(&_settingsLock);
            HostLog(L"camera %d: re-advertised as %s -> %ux%u@%u", _cameraIndex,
                    prof->displayName, active.width, active.height, active.fps);
        }
        // Republish the capability block from the now-known profile so the DLL
        // builds its media types from the live device (modes/formats/default
        // subtype), not whatever occupied the slot when Run() started.
        deviceregistry::PublishColdBlock(bus, _cameraIndex);
    };

    auto releaseCamera = [&]() {
        if (device)
        {
            device->Stop();   // sensor off, LED off, transfers cancelled, USB released
            device.reset();
        }
        // Refresh the slot map: drops devices that were unplugged and repairs
        // slots whose camera was replugged (stale libusb_device swapped for the
        // live one). The slot keeps the same index throughout.
        deviceregistry::Rescan();
        _fpsX10.store(0, std::memory_order_relaxed);
    };

    // Drains a UI settings snapshot. cameraLive: sensor settings are pushed to
    // the hardware immediately. Mode changes: in-place bus update while the
    // camera is off; deferred to the next sleep transition while streaming.
    auto drainSettings = [&](bool cameraLive) {
        if (!_settingsDirty.exchange(false, std::memory_order_acquire))
            return;
        Settings desired;
        {
            AcquireSRWLockShared(&_settingsLock);
            desired = _desired;
            ReleaseSRWLockShared(&_settingsLock);
        }

        if (!desired.SameMode(active))
        {
            if (!cameraLive)
            {
                // A RESOLUTION change invalidates the media types the Frame
                // Server cached when the camera was registered. An app that
                // negotiated the old size keeps asking for it, and DeliverSample
                // has nothing to scale from unless the new size happens to be an
                // exact 2:1 pair — so it fills black at full frame rate, which is
                // exactly how "the settings preview works but the camera app is
                // black" presents. Re-register so open clients drop and
                // re-acquire against the freshly advertised geometry; the DLL
                // rebuilds its media types from the bus format on reopen.
                //
                // This is the same rule the profile-change path below follows for
                // single-eye <-> SBS. It was missing here, so it only bit the
                // user-selectable modes (PS4 640x400 / 320x192, PS3 Eye's
                // thirteen), not the view switches.
                //
                // Frame rate alone does not need it: which types are DELIVERABLE
                // depends on the dimensions, and every rate is already advertised.
                const bool geometryChanged =
                    desired.width != active.width || desired.height != active.height;
                bus.UpdateFormat(desired.width, desired.height, desired.fps, 1);
                active.width = desired.width;
                active.height = desired.height;
                active.fps = desired.fps;
                _pendingMode.store(false, std::memory_order_relaxed);
                HostLog(L"mode changed to %ux%u@%u%s", active.width, active.height, active.fps,
                        geometryChanged ? L" (re-register: resolution change)" : L"");
                if (geometryChanged && vcamLive)
                {
                    unregisterVCam();
                    tryRegisterVCam();
                }
            }
            else
            {
                _pendingMode.store(true, std::memory_order_relaxed);
                HostLog(L"mode change queued until camera is idle");
            }
        }

        // One list, declared next to the fields themselves (common/Settings.h),
        // so a newly added control cannot be silently dropped here.
        const bool sensorChanged = !SensorControlsEqual(desired, active);
        CopySensorControls(desired, active);
        active.idleTimeoutMs = desired.idleTimeoutMs;

        if (cameraLive && sensorChanged && device)
            device->ApplySettings(active);

        AcquireSRWLockExclusive(&_settingsLock);
        _active = active;
        ReleaseSRWLockExclusive(&_settingsLock);
    };

    // Publish mic levels for the UI and, while recording, append to the WAV.
    // Called once per captured frame from the loop below.
    WavWriter micWav;
    audiorender::Renderer micRender;
    audiostatus::Writer    micStatus;
    std::wstring  micRenderWant;                // last device id we acted on
    std::wstring  micRenderPref;                // cached setting
    std::wstring  micRenderName;                // friendly name, for the status block
    bool          micKeepAwake = true;          // cached alongside micRenderPref
    ULONGLONG     micRenderChecked = 0;
    ULONGLONG     micStatusPublished = 0;         // last status-block publish
    auto pumpMicrophone = [&](ICameraDevice* dev)
    {
        const uint32_t ch = dev ? dev->AudioChannels() : 0;
        _micChannels.store(ch, std::memory_order_relaxed);

        // Created on FIRST AUDIO, not from the profile at thread start: the slot
        // may be empty when this thread starts and gain a PS4 later, and a
        // decision taken up there would then never be revisited. Keying on real
        // audio also keeps a PS3 Eye slot from clobbering the block with
        // channels=0 while the PS4 is live -- there is one block, not one per
        // slot, because there is one array.
        if (ch && !micStatus.Valid())
            micStatus.Create();

        // One clock read for both throttles below (settings re-read, status
        // publish); this runs per captured frame, i.e. up to 240 times a second.
        const ULONGLONG now = GetTickCount64();

        // Follow the selected output device. Start() is idempotent, so calling
        // it per frame costs a string compare; it only reopens on a change.
        {
            // Re-read at most once a second: a registry hit per video frame
            // would be 60/s for a value that changes when a human clicks.
            if (now - micRenderChecked > 1000)
            {
                micRenderChecked = now;
                micRenderPref = settings::LoadMicRenderDevice();
                micKeepAwake  = settings::LoadMicKeepAwake();
            }
            const std::wstring want = ch ? micRenderPref : std::wstring();
            // Take the wake hold only for a slot that really has a microphone,
            // has somewhere to send it, and is allowed to. Gating on `ch`
            // matters: without it a PS3 Eye slot would stream forever for audio
            // it does not carry. Clearing the hold does NOT stop routing -- it
            // just stops routing from being a reason to stay awake, so audio
            // still flows whenever something else has the camera open.
            _micHold.store(ch != 0 && !micRenderPref.empty() && micKeepAwake,
                           std::memory_order_relaxed);
            if (want != micRenderWant)
            {
                micRenderWant = want;
                micRenderName.clear();
                if (want.empty()) micRender.Stop();
                else
                {
                    micRender.Start(want, audiorender::Downmix::Mono);
                    for (const auto& e : audiorender::ListEndpoints())
                        if (e.id == want) { micRenderName = e.name; break; }
                }
            }
        }
        if (dev)
            _micDropouts.store(dev->AudioDropouts(), std::memory_order_relaxed);
        if (ch == 0)
        {
            if (micWav.IsOpen())
            {
                micWav.Close();
                _micRecording.store(false, std::memory_order_release);
            }
            // Never hold an output device with nothing to send it.
            if (micRender.Running()) { micRender.Stop(); micRenderWant.clear(); }
            return;
        }

        float levels[kMicMaxChannels] = {};
        if (dev->AudioLevels(levels, kMicMaxChannels))
            for (uint32_t c = 0; c < ch && c < kMicMaxChannels; ++c)
                _micLevel[c].store((uint32_t)(levels[c] * 10000.0f), std::memory_order_relaxed);

        const bool want = _micRecording.load(std::memory_order_acquire);
        if (want && !micWav.IsOpen())
        {
            if (!micWav.Open(_micRecPath.c_str(), dev->AudioSampleRate(), (uint16_t)ch))
            {
                HostLog(L"mic: could not open the recording file");
                _micRecording.store(false, std::memory_order_release);
                return;
            }
            HostLog(L"mic: recording %u ch @ %u Hz", ch, dev->AudioSampleRate());
        }
        else if (!want && micWav.IsOpen())
        {
            const double secs = micWav.Seconds();
            micWav.Close();   // patches the RIFF/data sizes; can fail too
            if (micWav.WriteFailed())
                HostLog(L"mic: recording TRUNCATED after %.1f s -- the file is short "
                        L"(disk full, or the volume went away)", secs);
            else
                HostLog(L"mic: stopped after %.1f s", secs);
        }

        // Always drain, recording or not: an undrained ring only discards its
        // oldest second, but draining keeps the levels tracking real time.
        static const uint32_t kChunk = 2048;
        int16_t buf[kChunk * kMicMaxChannels];
        for (;;)
        {
            const uint32_t got = dev->ReadAudio(buf, kChunk);
            if (!got)
                break;
            // Out of the tray: the same samples, rendered into whatever output
            // the user chose. Point it at a virtual cable and the array is a
            // real microphone in every app. Push() is wait-free and returns
            // immediately when no device is selected, so the cost here is a
            // predictable branch when the feature is off.
            micRender.Push(buf, got, ch, dev->AudioMeasuredRateMilliHz());

            if (micWav.IsOpen())
            {
                micWav.Write(buf, got);
                if (micWav.WriteFailed())
                {
                    // A full disk will not un-fill itself.
                    HostLog(L"mic: write failed -- stopping the recording");
                    micWav.Close();
                    _micRecording.store(false, std::memory_order_release);
                    break;
                }
            }
            if (got < kChunk)
                break;
        }
        if (micWav.IsOpen())
            _micRecSecs.store((uint32_t)micWav.Seconds(), std::memory_order_relaxed);

        // Publish microphone health and renderer drift at 10 Hz for status
        // readers. This avoids per-frame copies while remaining well within
        // kFreshMs for staleness detection.
        if (micStatus.Valid() && now - micStatusPublished >= 100)
        {
            micStatusPublished = now;
            audiostatus::Block b{};
            b.slot     = _cameraIndex;
            b.channels = ch;
            b.nominalRate = dev->AudioSampleRate();
            b.measuredRateMilliHz = dev->AudioMeasuredRateMilliHz();
            b.dropouts = _micDropouts.load(std::memory_order_relaxed);
            for (uint32_t c = 0; c < 4 && c < ch; ++c)
                b.levels[c] = _micLevel[c].load(std::memory_order_relaxed);
            const audiorender::Status rs = micRender.GetStatus();
            b.renderActive     = rs.active ? 1u : 0u;
            b.renderFillMs     = rs.fillMs;
            b.renderPpm        = rs.correctionPpm;
            b.renderUnderruns  = (uint32_t)rs.underruns;
            b.renderOverruns   = (uint32_t)rs.overruns;
            b.endpointRate     = rs.endpointRate;
            b.endpointChannels = rs.endpointChans;
            wcsncpy_s(b.endpointName, micRenderName.c_str(), _TRUNCATE);
            micStatus.Publish(b);
        }
    };

    auto applyPendingMode = [&]() {  // call only with the camera off
        if (!_pendingMode.load(std::memory_order_relaxed))
            return;
        _settingsDirty.store(true, std::memory_order_release);
        drainSettings(false);
    };

    // SlotEmpty (device physically absent) is distinguished from InitFailed
    // (device present but USB setup failed): the former unregisters the
    // virtual camera, the latter keeps it and retries.
    enum class WakeResult { Ok, SlotEmpty, InitFailed };
    auto wakeCamera = [&]() -> WakeResult {
        device = deviceregistry::Acquire(_cameraIndex);
        if (!device)
            return WakeResult::SlotEmpty;
        const VideoMode mode{ active.width, active.height, active.fps };
        if (!device->Init(mode))   // releases the USB handle on failure
        {
            device.reset();
            return WakeResult::InitFailed;
        }
        // A device may not be able to honour the requested mode (the PS4's split
        // pair shares one hardware stream, so the non-owning half adopts the
        // owner's geometry). Advertise what it ACTUALLY produces, or the
        // FrameBus is sized for frames that never arrive.
        const VideoMode actual = device->ActualMode(mode);
        if (actual.width != active.width || actual.height != active.height ||
            actual.fps != active.fps)
        {
            HostLog(L"camera %d: device produces %ux%u@%u (asked %ux%u@%u) — re-advertising",
                    _cameraIndex, actual.width, actual.height, actual.fps,
                    active.width, active.height, active.fps);
            active.width = actual.width; active.height = actual.height; active.fps = actual.fps;
            bus.UpdateFormat(active.width, active.height, active.fps, 1);
            // NOTE: this changes the advertised geometry without re-registering,
            // so it carries the same staleness risk drainSettings guards against
            // — a client that already negotiated the requested size would go
            // black. Deliberately not re-registered here: we are mid-wake with a
            // client waiting, and dropping it now risks a wake/sleep oscillation.
            // It only diverges when the two halves of a PS4 split pair are set to
            // DIFFERENT modes; if that turns out to bite, the fix is to reconcile
            // on the next Asleep pass, where re-registering is free.
            // The DLL now traces "BLACK -- negotiated AxB but bus is CxD" once,
            // so this is diagnosable instead of silent.
        }
        device->ApplySettings(active);
        if (!device->Start())
        {
            device.reset();
            return WakeResult::InitFailed;
        }
        return WakeResult::Ok;
    };

    // ---- state machine ------------------------------------------------------
    enum class Phase { Asleep, Waking, Streaming };
    Phase phase = Phase::Asleep;

    // Device profile last advertised into the ColdBlock for this slot. nullptr
    // forces the first occupied Asleep pass to (re)advertise, covering a device
    // that finished USB enumeration after Run() published the start-time block.
    const DeviceProfile* publishedProfile = nullptr;

    int consecutiveTimeouts = 0;
    uint32_t framesInWindow = 0;
    ULONGLONG windowStart = GetTickCount64();

    // PS4 wedged-session detection: the OV580 yields one clean streaming session
    // per physical plug. If a later session opens (wakeCamera succeeds) but never
    // delivers a frame, it is wedged and only a replug recovers it. sawFrameSinceWake
    // distinguishes "streamed then stalled" (transient/device-lost) from "never
    // streamed" (wedged); ps4BarrenStreams counts consecutive never-streamed sessions
    // so a single hiccup doesn't trip the "needs replug" notice.
    bool sawFrameSinceWake = false;
    bool ps4Slot          = false;   // cached at wake from the device's transport
    int  ps4BarrenStreams = 0;

    // Watch the settings key so an out-of-process change to the microphone's
    // render target wakes an idle camera. EVENT-DRIVEN on purpose: the Asleep
    // wait is INFINITE once a camera is present and its vcam is live, so a poll
    // would be the only alternative, and this loop is deliberately free of idle
    // polling. RegNotifyChangeKeyValue is one handle and zero wakeups until
    // something actually changes.
    HKEY   settingsWatchKey = nullptr;
    HANDLE settingsChanged  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto armSettingsWatch = [&] {
        if (!settingsChanged) return;
        if (!settingsWatchKey &&
            RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\PSCam4Win", 0,
                          KEY_NOTIFY, &settingsWatchKey) != ERROR_SUCCESS)
        { settingsWatchKey = nullptr; return; }
        ResetEvent(settingsChanged);
        // Subtree FALSE: MicRenderDevice is a value on this key, while every
        // per-camera setting lives in a Camera%d SUBkey. Watching the subtree
        // would wake all eight sleeping controllers on every slider persist,
        // for a value none of them read.
        RegNotifyChangeKeyValue(settingsWatchKey, FALSE, REG_NOTIFY_CHANGE_LAST_SET,
                                settingsChanged, TRUE);
    };
    armSettingsWatch();

    HANDLE asleepWaits[5] = { _stopEvent, _cmdEvent, control.WakeEvent(), _rescanEvent,
                              settingsChanged };
    const DWORD asleepWaitCount = settingsChanged ? 5 : 4;
    HANDLE briefWaits[2]  = { _stopEvent, _cmdEvent };

    struct WatchGuard {
        HKEY* k; HANDLE* e;
        ~WatchGuard() { if (*k) RegCloseKey(*k); if (*e) CloseHandle(*e); }
    } watchGuard{ &settingsWatchKey, &settingsChanged };

    bool stopping = false;
    while (!stopping)
    {
        switch (phase)
        {
        case Phase::Asleep:
        {
            // Lost-wakeup-free order: reset, THEN re-check freshness.
            control.ResetWake();
            drainSettings(false);
            applyPendingMode();

            // Reconcile the virtual camera with slot occupancy: registered
            // while a physical camera is attached, gone otherwise.
            bool present = false;
            if (createVCam)
            {
                // ProfileForSlot is non-null iff the slot is occupied AND tells us
                // which device occupies it, in a single enumeration. Re-advertise
                // whenever the occupant changes (including empty -> present) so the
                // DLL always builds media types from the live device's profile.
                const DeviceProfile* prof = deviceregistry::ProfileForSlot(_cameraIndex);
                present = (prof != nullptr);
                if (present && prof != publishedProfile)
                {
                    publishedProfile = prof;
                    readvertiseSlot(prof);
                }
                if (present && !vcamLive)
                    tryRegisterVCam();
                else if (!present && vcamLive)
                    unregisterVCam();

                // Mic-only wake has to be decided HERE as well as in the capture
                // pump, or it could never start: the pump only runs while
                // streaming, so a sleeping camera would wait for a hold that
                // waits for the camera. Asleep we have no device to ask, so the
                // slot's PROFILE answers whether it has an array at all.
                _micHold.store(present && prof &&
                               (prof->controlMask & CTRL_MICARRAY) != 0 &&
                               !settings::LoadMicRenderDevice().empty() &&
                               settings::LoadMicKeepAwake(),
                               std::memory_order_relaxed);
            }

            if (vcamLive && clientFresh())
            {
                phase = Phase::Waking;
                break;
            }
            if (createVCam)
                SetState(!present ? State::CameraMissing
                                  : (vcamLive ? State::Asleep : State::VCamFailed));

            DWORD timeout = INFINITE;
            if (createVCam)
            {
                const ULONGLONG now = GetTickCount64();
                if (!present)
                    timeout = 5000;  // fallback arrival poll (device notification may race libusb)
                else if (!vcamLive)
                    timeout = vcamRetryDue > now ? static_cast<DWORD>(vcamRetryDue - now) : 0;
            }

            const DWORD r = WaitForMultipleObjects(asleepWaitCount, asleepWaits, FALSE, timeout);
            if (r == WAIT_OBJECT_0)            // stop
                stopping = true;
            else if (r == WAIT_OBJECT_0 + 4)   // settings changed out of process
                armSettingsWatch();            // re-arm; the loop top re-reads
            // Settings command, wake ping, device change, or timeout: loop —
            // the top of the Asleep pass re-evaluates everything.
            break;
        }

        case Phase::Waking:
        {
            drainSettings(false);
            applyPendingMode();
            if (!clientFresh())
            {
                phase = Phase::Asleep;  // client went away while we were down
                break;
            }
            SetState(State::Waking);
            const WakeResult wake = wakeCamera();
            if (wake == WakeResult::Ok)
            {
                HostLog(L"camera %d awake: %ux%u@%u", _cameraIndex, active.width, active.height, active.fps);
                SetState(State::Streaming);
                consecutiveTimeouts = 0;
                framesInWindow = 0;
                windowStart = GetTickCount64();
                sawFrameSinceWake = false;   // arm the wedged-session check
                ps4Slot = device->Profile().transport == TransportClass::Usb_Ps4;
                phase = Phase::Streaming;
                break;
            }
            if (wake == WakeResult::SlotEmpty)
            {
                // Device physically gone: take the virtual camera offline so
                // apps stop seeing a dead "PS3 Eye" entry.
                unregisterVCam();
                SetState(State::CameraMissing);
                phase = Phase::Asleep;
                break;
            }
            SetState(State::CameraMissing);
            // Device present but USB setup failed: retry every 2s while a
            // client keeps asking; drop to sleep otherwise.
            const DWORD r = WaitForMultipleObjects(2, briefWaits, FALSE, 2000);
            if (r == WAIT_OBJECT_0)
                stopping = true;
            break;
        }

        case Phase::Streaming:
        {
            if (WaitForSingleObject(_stopEvent, 0) == WAIT_OBJECT_0)
            {
                stopping = true;
                break;
            }
            // The DLL publishes which format the active client negotiated; only
            // an MJPEG client needs the JFIF sidecar. mask==0 (no/stale client)
            // -> wantJpeg=false. One plain aligned load + branch; the
            // PS3 path always reports no jpeg and falls through to Publish().
            const bool wantJpeg =
                (control.ConsumerMask() & controlbus::kConsumeMJPEG) != 0;

            // A PS4 that has not yet produced a frame THIS session may just be slow to
            // start (sensor/ISP warm-up, host load), not wedged — give its first frame
            // a generous window before ending the session; once a frame has arrived (or
            // for non-PS4 devices) the normal ~2s stall window applies. ps4Slot is
            // cached at wake so QueryPs4Slot stays off this path.
            const int stallLimit = (ps4Slot && !sawFrameSinceWake) ? 10 : 4;  // x 500ms

            AcquiredFrame frame{};
            if (device->AcquireFrame(frame, 500, wantJpeg))
            {
                consecutiveTimeouts = 0;
                sawFrameSinceWake = true;   // a real frame: not a wedged session
                ps4BarrenStreams = 0;
                if (frame.jpeg && frame.jpegBytes)
                    bus.PublishWithJpeg(frame.yuy2, frame.yuy2Bytes, frame.jpeg, frame.jpegBytes);
                else
                    bus.Publish(frame.yuy2, frame.yuy2Bytes);

                // The PS4's mic array rides inside the video frame, so its audio
                // becomes available exactly when a frame does. Pumping it here
                // keeps it on the camera thread (the UI only reads atomics).
                pumpMicrophone(device.get());

                ++framesInWindow;
                const ULONGLONG now = GetTickCount64();
                if (now - windowStart >= 2000)
                {
                    _fpsX10.store(static_cast<uint32_t>(framesInWindow * 10000ull / (now - windowStart)),
                                  std::memory_order_relaxed);
                    framesInWindow = 0;
                    windowStart = now;
                }
            }
            else if (++consecutiveTimeouts >= stallLimit)
            {
                releaseCamera();

                // A PS4 that woke (iso submitted) yet delivered no frames across two
                // full first-frame windows is a wedged post-firmware session: retrying
                // in place can't recover it (only a power-cycle reloads the OV580
                // firmware). After a couple of barren sessions, surface "needs replug"
                // and PARK on a device-change instead of spinning Waking<->Streaming
                // forever. (ps4Slot is cached at wake from the device's transport.)
                if (ps4Slot && !sawFrameSinceWake && ++ps4BarrenStreams >= 2)
                {
                    HostLog(L"camera %d: PS4 firmware session wedged (no frames) -- needs replug",
                            _cameraIndex);
                    bus.PublishBlack();
                    SetState(State::Ps4NeedsReplug);   // tray toast + Settings notice
                    HANDLE wedgedWaits[3] = { _stopEvent, _rescanEvent, _cmdEvent };
                    if (WaitForMultipleObjects(3, wedgedWaits, FALSE, INFINITE) == WAIT_OBJECT_0)
                        stopping = true;
                    // A replug/device-change (or a settings poke) woke us: re-attempt.
                    // The registry retires the wedged source for an absent port path,
                    // so a fresh plug firmwares cleanly; a still-wedged device just
                    // re-detects and re-parks.
                    ps4BarrenStreams = 0;
                    phase = Phase::Waking;
                    break;
                }

                HostLog(L"camera %d stopped delivering frames -- device lost?", _cameraIndex);
                phase = Phase::Waking;  // immediate retry covers replug
                break;
            }

            drainSettings(true);

            // A PS4 view switch (Left/Right/SBS) changes ProfileForSlot without a
            // Settings.width change, so the Asleep re-advertise path never runs
            // while a client streams. The
            // Settings dialog fires a device-change (RescanAllControllers) on a
            // view switch; on that signal, re-check the profile and, if it differs,
            // re-advertise + re-create the device live. Left<->Right keep the same
            // geometry (client continues); to/from SBS re-advertises (a client may
            // need to reopen). The auto-reset event keeps this off the hot path.
            if (WaitForSingleObject(_rescanEvent, 0) == WAIT_OBJECT_0)
            {
                const DeviceProfile* prof = deviceregistry::ProfileForSlot(_cameraIndex);
                if (prof && prof != publishedProfile)
                {
                    // Prefer a LIVE view switch (PS4 Left/Right/SBS): the device
                    // recomposes the same streaming frame with no iso restart, so
                    // it never goes black or degrades. Re-advertise the geometry
                    // (changes only for SBS). Fall back to a full re-acquire only if
                    // the device can't switch in place.
                    //
                    // A resolution change (single-eye 1280x800 <-> SBS 2560x800)
                    // CANNOT be followed by an app mid-stream: its media type was
                    // fixed at open, so it would keep pulling the old size and go
                    // blank. Left<->Right keep the same geometry and stream straight
                    // through; for a geometry change we re-register the vcam after
                    // re-advertising, so any open client drops and re-acquires at the
                    // new resolution (the DLL rebuilds its media types from the fresh
                    // ColdBlock on reopen).
                    const bool geometryChanged =
                        !publishedProfile ||
                        prof->defaultMode.width  != publishedProfile->defaultMode.width ||
                        prof->defaultMode.height != publishedProfile->defaultMode.height;

                    const deviceregistry::Ps4SlotInfo q = deviceregistry::QueryPs4Slot(_cameraIndex);
                    if (device && q.isPs4 && device->SetView(static_cast<int>(q.view)))
                    {
                        HostLog(L"camera %d: live view switch -> %s%s", _cameraIndex,
                                prof->displayName, geometryChanged ? L" (re-register: resolution change)" : L"");
                        publishedProfile = prof;
                        readvertiseSlot(prof);   // ColdBlock + bus geometry update
                        if (geometryChanged)
                        {
                            unregisterVCam();    // force open clients to re-acquire
                            tryRegisterVCam();    // at the new advertised resolution
                        }
                    }
                    else
                    {
                        HostLog(L"camera %d: profile changed -> %s (re-acquire)", _cameraIndex, prof->displayName);
                        publishedProfile = prof;
                        readvertiseSlot(prof);
                        releaseCamera();
                        bus.PublishBlack();
                        phase = Phase::Waking;
                        break;
                    }
                }
            }

            // Go to sleep when there are no clients, OR when the preview is
            // the only consumer and a mode change is pending (applying it
            // requires releasing the camera; preview-hold re-wakes it
            // immediately afterwards with the new format).
            const bool previewOnlyModeChange =
                _pendingMode.load(std::memory_order_relaxed) &&
                IsPreviewOnly();

            if (previewOnlyModeChange || !clientFresh())
            {
                if (previewOnlyModeChange)
                    HostLog(L"camera %d: applying pending mode change for preview",
                            _cameraIndex);
                else
                    HostLog(L"camera %d: no clients for %ums and preview closed -- going to sleep",
                            _cameraIndex, active.idleTimeoutMs);
                // Tear the audio side down BEFORE the camera goes: the pump is
                // the only thing that ever stops the renderer, and it only runs
                // while streaming. Without this the render thread survives the
                // sleep with nothing feeding it -- it starves on every callback
                // (1,440 underruns in one measured 14 s sleep) and keeps the
                // user's output device open while idle, which AudioRender.h
                // exists partly to avoid. Passing nullptr is the same "no audio
                // any more" path a camera without a microphone takes.
                pumpMicrophone(nullptr);
                releaseCamera();
                bus.PublishBlack();
                applyPendingMode();
                phase = Phase::Asleep;
            }
            break;
        }
        }
    }

    // ---- ordered teardown ---------------------------------------------------
    releaseCamera();
    bus.PublishBlack();
    unregisterVCam();
    control.Close();
    bus.Close();
    MFShutdown();
    CoUninitialize();
    HostLog(L"camera %d thread exited", _cameraIndex);
}


// ---------------------------------------------------------------------------
// Microphone accessors. Called on the UI thread; everything here is either an
// atomic or written before the flag the camera thread acquires.
// ---------------------------------------------------------------------------

void CaptureController::MicLevels(float* out, uint32_t count) const
{
    for (uint32_t c = 0; c < count && c < kMicMaxChannels; ++c)
        out[c] = _micLevel[c].load(std::memory_order_relaxed) / 10000.0f;
}

std::wstring CaptureController::MicStartRecording()
{
    if (_micChannels.load(std::memory_order_relaxed) == 0)
        return std::wstring();

    wchar_t dir[MAX_PATH] = L"";
    if (FAILED(SHGetFolderPathW(nullptr, CSIDL_MYVIDEO, nullptr, 0, dir)))
        GetTempPathW(MAX_PATH, dir);

    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\PSCam4Win-mic-%04u%02u%02u-%02u%02u%02u.wav",
               dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    // Path BEFORE the flag: the camera thread reads it only after it observes
    // _micRecording, and the release/acquire pair orders the two writes.
    _micRecPath = path;
    _micRecSecs.store(0, std::memory_order_relaxed);
    _micRecording.store(true, std::memory_order_release);
    return _micRecPath;
}
