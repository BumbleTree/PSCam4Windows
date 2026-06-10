#include "CaptureController.h"

#include <mfapi.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdarg>
#include <memory>

#include "ps3eye.h"
#include "mfvirtualcamera_min.h"
#include "../common/FrameBus.h"
#include "../common/ControlBus.h"
#include "../common/VCamGuids.h"

using Microsoft::WRL::ComPtr;

void HostLog(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    if (GetConsoleWindow())
        wprintf(L"%s\n", buf);
    wchar_t line[560];
    _snwprintf_s(line, _TRUNCATE, L"PS3EyeVCamTray: %s\n", buf);
    OutputDebugStringW(line);
}

namespace
{

// BT.601 limited-range BGRA -> NV12, 2x2 blocks (chroma from the 4-pixel mean).
void BgraToNv12(const uint8_t* bgra, uint8_t* nv12, uint32_t w, uint32_t h)
{
    uint8_t* yPlane = nv12;
    uint8_t* uvPlane = nv12 + static_cast<size_t>(w) * h;

    for (uint32_t y = 0; y < h; y += 2)
    {
        const uint8_t* row0 = bgra + static_cast<size_t>(y) * w * 4;
        const uint8_t* row1 = row0 + static_cast<size_t>(w) * 4;
        uint8_t* y0 = yPlane + static_cast<size_t>(y) * w;
        uint8_t* y1 = y0 + w;
        uint8_t* uv = uvPlane + static_cast<size_t>(y / 2) * w;

        for (uint32_t x = 0; x < w; x += 2)
        {
            int rSum = 0, gSum = 0, bSum = 0;
            const uint8_t* px[4] = { row0 + x * 4, row0 + (x + 1) * 4,
                                     row1 + x * 4, row1 + (x + 1) * 4 };
            uint8_t* yOut[4] = { y0 + x, y0 + x + 1, y1 + x, y1 + x + 1 };
            for (int i = 0; i < 4; ++i)
            {
                const int b = px[i][0], g = px[i][1], r = px[i][2];
                rSum += r; gSum += g; bSum += b;
                *yOut[i] = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
            }
            const int r = rSum / 4, g = gSum / 4, b = bSum / 4;
            uv[x]     = static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
            uv[x + 1] = static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
        }
    }
}

// Push every sensor-level setting to the camera (order-safe live or pre-start).
void ApplySensorSettings(const ps3eye::PS3EYECam::PS3EYERef& eye, const Settings& s)
{
    eye->setFlip(s.flipH, s.flipV);
    eye->setAutoWhiteBalance(s.autoWhiteBalance);
    if (s.autoGain)
    {
        eye->setAutogain(true);
    }
    else
    {
        eye->setAutogain(false);  // also re-applies cached gain/exposure
        eye->setGain(static_cast<uint8_t>(s.gain));
        eye->setExposure(static_cast<uint8_t>(s.exposure));
    }
}

} // namespace

// ---------------------------------------------------------------------------

bool CaptureController::Start(HWND notifyWnd, UINT notifyMsg)
{
    _notifyWnd = notifyWnd;
    _notifyMsg = notifyMsg;
    {
        AcquireSRWLockExclusive(&_settingsLock);
        _desired = settings::Load();
        _active = _desired;
        ReleaseSRWLockExclusive(&_settingsLock);
    }
    _stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    _cmdEvent  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!_stopEvent || !_cmdEvent)
        return false;
    _thread = CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr);
    return _thread != nullptr;
}

void CaptureController::Stop()
{
    if (_stopEvent)
        SetEvent(_stopEvent);
    if (_thread)
    {
        WaitForSingleObject(_thread, 15000);
        CloseHandle(_thread);
        _thread = nullptr;
    }
    if (_stopEvent) { CloseHandle(_stopEvent); _stopEvent = nullptr; }
    if (_cmdEvent)  { CloseHandle(_cmdEvent);  _cmdEvent = nullptr; }
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

Settings CaptureController::ActiveSettings() const
{
    AcquireSRWLockShared(&_settingsLock);
    Settings s = _active;
    ReleaseSRWLockShared(&_settingsLock);
    return s;
}

void CaptureController::SetState(State s)
{
    if (_state.exchange(s, std::memory_order_relaxed) != s && _notifyWnd)
        PostMessage(_notifyWnd, _notifyMsg, static_cast<WPARAM>(static_cast<int>(s)), 0);
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
    if (!bus.Create(active.width, active.height, active.fps, 1))
    {
        HostLog(L"FATAL: FrameBus creation failed (win32=%lu) -- not elevated?", bus.LastError());
        SetState(State::Fatal);
        MFShutdown();
        CoUninitialize();
        return;
    }
    bus.PublishBlack();

    controlbus::Host control;
    if (!control.Create())
    {
        HostLog(L"FATAL: ControlBus creation failed (win32=%lu)", GetLastError());
        SetState(State::Fatal);
        MFShutdown();
        CoUninitialize();
        return;
    }

    // ---- virtual camera registration (retried while it fails) -------------
    HMODULE sensorGroup = LoadLibraryW(L"mfsensorgroup.dll");
    auto createVCam = sensorGroup
        ? reinterpret_cast<PFN_MFCreateVirtualCamera>(
              GetProcAddress(sensorGroup, "MFCreateVirtualCamera"))
        : nullptr;

    ComPtr<IMFVirtualCamera> vcam;
    bool vcamLive = false;
    ULONGLONG vcamRetryDue = 0;  // GetTickCount64 deadline for next attempt
    int vcamAttempts = 0;

    auto tryRegisterVCam = [&]() {
        if (vcamLive || !createVCam || GetTickCount64() < vcamRetryDue)
            return;
        ++vcamAttempts;
        HRESULT hr = createVCam(MFVirtualCameraType_SoftwareCameraSource,
                                MFVirtualCameraLifetime_Session,
                                MFVirtualCameraAccess_CurrentUser,
                                kVCamFriendlyName, kVCamClsidString, nullptr, 0, &vcam);
        if (SUCCEEDED(hr))
            hr = vcam->Start(nullptr);
        if (SUCCEEDED(hr))
        {
            vcamLive = true;
            HostLog(L"virtual camera registered and started");
        }
        else
        {
            vcam.Reset();
            // Right after logon MF may not be ready yet: 5 quick attempts at
            // 2s, then every 30s forever.
            vcamRetryDue = GetTickCount64() + (vcamAttempts < 5 ? 2000 : 30000);
            HostLog(L"virtual camera registration failed 0x%08X (attempt %d)", hr, vcamAttempts);
            SetState(State::VCamFailed);
        }
    };
    if (!createVCam)
    {
        HostLog(L"FATAL: MFCreateVirtualCamera unavailable -- Windows 11 22000+ required");
        SetState(State::Fatal);
    }
    else
    {
        tryRegisterVCam();
    }

    // ---- camera state ------------------------------------------------------
    ps3eye::PS3EYECam::PS3EYERef eye;

    const uint32_t maxBgraBytes = framebus::kMaxWidth * framebus::kMaxHeight * 4;
    std::unique_ptr<uint8_t[]> bgra(new uint8_t[maxBgraBytes]);
    std::unique_ptr<uint8_t[]> nv12(new uint8_t[framebus::kMaxFrameBytes]);

    auto releaseCamera = [&]() {
        if (eye)
        {
            eye->stop();      // sensor off, LED off, URB thread joins
            eye.reset();
        }
        // The driver's static device list holds the last reference; refreshing
        // it destroys the object -> close_usb() -> USB interface released.
        ps3eye::PS3EYECam::getDevices(true);
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
                bus.UpdateFormat(desired.width, desired.height, desired.fps, 1);
                active.width = desired.width;
                active.height = desired.height;
                active.fps = desired.fps;
                _pendingMode.store(false, std::memory_order_relaxed);
                HostLog(L"mode changed to %ux%u@%u", active.width, active.height, active.fps);
            }
            else
            {
                _pendingMode.store(true, std::memory_order_relaxed);
                HostLog(L"mode change queued until camera is idle");
            }
        }

        const bool sensorChanged =
            desired.flipH != active.flipH || desired.flipV != active.flipV ||
            desired.autoGain != active.autoGain || desired.gain != active.gain ||
            desired.exposure != active.exposure ||
            desired.autoWhiteBalance != active.autoWhiteBalance;

        active.flipH = desired.flipH;       active.flipV = desired.flipV;
        active.autoGain = desired.autoGain; active.gain = desired.gain;
        active.exposure = desired.exposure; active.autoWhiteBalance = desired.autoWhiteBalance;
        active.idleTimeoutMs = desired.idleTimeoutMs;

        if (cameraLive && sensorChanged && eye)
            ApplySensorSettings(eye, active);

        AcquireSRWLockExclusive(&_settingsLock);
        _active = active;
        ReleaseSRWLockExclusive(&_settingsLock);
    };

    auto applyPendingMode = [&]() {  // call only with the camera off
        if (!_pendingMode.load(std::memory_order_relaxed))
            return;
        _settingsDirty.store(true, std::memory_order_release);
        drainSettings(false);
    };

    // Returns true when the physical camera is up and streaming.
    auto wakeCamera = [&]() -> bool {
        const auto& devices = ps3eye::PS3EYECam::getDevices(true);
        if (devices.empty())
            return false;
        eye = devices[0];
        if (!eye->init(active.width, active.height, static_cast<uint16_t>(active.fps),
                       ps3eye::PS3EYECam::EOutputFormat::BGRA))
        {
            eye.reset();
            return false;
        }
        ApplySensorSettings(eye, active);
        eye->start();
        return true;
    };

    // ---- state machine ------------------------------------------------------
    enum class Phase { Asleep, Waking, Streaming };
    Phase phase = Phase::Asleep;
    SetState(vcamLive ? State::Asleep : GetState());

    int consecutiveTimeouts = 0;
    uint32_t framesInWindow = 0;
    ULONGLONG windowStart = GetTickCount64();

    HANDLE asleepWaits[3] = { _stopEvent, _cmdEvent, control.WakeEvent() };
    HANDLE briefWaits[2]  = { _stopEvent, _cmdEvent };

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
            if (vcamLive && control.ActivityAgeMs() < active.idleTimeoutMs)
            {
                phase = Phase::Waking;
                break;
            }
            SetState(vcamLive ? State::Asleep : GetState());

            DWORD timeout = INFINITE;
            if (!vcamLive)
            {
                const ULONGLONG now = GetTickCount64();
                timeout = vcamRetryDue > now ? static_cast<DWORD>(vcamRetryDue - now) : 0;
            }
            const DWORD r = WaitForMultipleObjects(3, asleepWaits, FALSE, timeout);
            if (r == WAIT_OBJECT_0)            // stop
                stopping = true;
            else if (r == WAIT_OBJECT_0 + 1)   // settings command
            {
                drainSettings(false);
                applyPendingMode();
            }
            else if (r == WAIT_OBJECT_0 + 2)   // wake ping from the DLL
                phase = Phase::Waking;
            else                               // timeout -> vcam retry due
                tryRegisterVCam();
            break;
        }

        case Phase::Waking:
        {
            drainSettings(false);
            applyPendingMode();
            if (control.ActivityAgeMs() >= active.idleTimeoutMs)
            {
                phase = Phase::Asleep;  // client went away while we were down
                break;
            }
            SetState(State::Waking);
            if (wakeCamera())
            {
                HostLog(L"camera awake: %ux%u@%u", active.width, active.height, active.fps);
                SetState(State::Streaming);
                consecutiveTimeouts = 0;
                framesInWindow = 0;
                windowStart = GetTickCount64();
                phase = Phase::Streaming;
                break;
            }
            SetState(State::CameraMissing);
            // Retry every 2s while a client keeps asking; drop to sleep otherwise.
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
            if (eye->getFrame(bgra.get(), 500))
            {
                consecutiveTimeouts = 0;
                BgraToNv12(bgra.get(), nv12.get(), active.width, active.height);
                bus.Publish(nv12.get(), framebus::Nv12Bytes(active.width, active.height));

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
            else if (++consecutiveTimeouts >= 4)
            {
                HostLog(L"camera stopped delivering frames -- device lost?");
                releaseCamera();
                phase = Phase::Waking;  // immediate retry covers replug
                break;
            }

            drainSettings(true);

            if (control.ActivityAgeMs() >= active.idleTimeoutMs)
            {
                HostLog(L"no clients for %ums -- camera going to sleep", active.idleTimeoutMs);
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
    if (vcam)
    {
        vcam->Stop();
        vcam->Remove();
        vcam->Shutdown();
        vcam.Reset();
    }
    control.Close();
    bus.Close();
    MFShutdown();
    CoUninitialize();
    HostLog(L"camera thread exited");
}
