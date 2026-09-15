#include "MicWatch.h"

#include <initguid.h>   // DEFINE the PROPERTYKEYs used below, don't just declare
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <atomic>
#include <thread>

#include "HostLog.h"

namespace
{
    // The PS3 Eye's audio function names itself this. Matching on the fragment
    // rather than the whole string survives Windows' "(2- ...)" disambiguation
    // prefix, which appears whenever the camera has been seen on two ports.
    const wchar_t kPs3MicName[] = L"B4.09.24.1";

    std::atomic<int>  g_last{ static_cast<int>(micwatch::Health::Unknown) };
    std::atomic<bool> g_inFlight{ false };
    std::atomic<bool> g_reported{ false };

    // The probe thread is OWNED, not detached. It holds COM and runs for up to
    // 1.2 s, so a detached one can still be inside WASAPI while the process
    // tears its runtime down. At most one exists at a time (g_inFlight), and
    // Shutdown joins it. UI thread only.
    std::thread g_probe;

    IMMDevice* FindPs3Mic()
    {
        IMMDeviceEnumerator* en = nullptr;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en)
            return nullptr;

        IMMDeviceCollection* col = nullptr;
        IMMDevice* found = nullptr;
        // ACTIVE only: a camera that has been plugged into several ports leaves
        // unplugged endpoint records behind, and one of those would answer for a
        // device that is not there.
        if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) && col)
        {
            UINT n = 0;
            col->GetCount(&n);
            for (UINT i = 0; i < n && !found; ++i)
            {
                IMMDevice* d = nullptr;
                if (FAILED(col->Item(i, &d)) || !d)
                    continue;
                IPropertyStore* ps = nullptr;
                bool match = false;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps)
                {
                    PROPVARIANT pv;
                    PropVariantInit(&pv);
                    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                        match = wcsstr(pv.pwszVal, kPs3MicName) != nullptr;
                    PropVariantClear(&pv);
                    ps->Release();
                }
                if (match) found = d; else d->Release();
            }
            col->Release();
        }
        en->Release();
        return found;
    }

    micwatch::Health ProbePs3Mic()
    {
        IMMDevice* dev = FindPs3Mic();
        if (!dev)
            return micwatch::Health::Unknown;   // no camera, or no endpoint yet

        IAudioClient* ac = nullptr;
        micwatch::Health result = micwatch::Health::Unknown;

        // SHARED deliberately: exclusive would evict whatever app is using the
        // mic, and this is a background health check, not a recording. A wedged
        // array reads empty in shared mode too -- that was measured.
        WAVEFORMATEX* wfx = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) && ac &&
            SUCCEEDED(ac->GetMixFormat(&wfx)) && wfx &&
            SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10'000'000, 0, wfx, nullptr)))
        {
            IAudioCaptureClient* cap = nullptr;
            if (SUCCEEDED(ac->GetService(__uuidof(IAudioCaptureClient), (void**)&cap)) && cap)
            {
                ac->Start();
                long long frames = 0;
                // 1.2 s: the suite's 1.5 s window never saw a healthy array take
                // more than a few tens of ms to produce its first packet, and a
                // wedged one produces nothing however long you wait.
                const ULONGLONG until = GetTickCount64() + 1200;
                while (GetTickCount64() < until && frames == 0)
                {
                    UINT32 avail = 0;
                    if (FAILED(cap->GetNextPacketSize(&avail)))
                        break;
                    if (!avail) { Sleep(5); continue; }
                    BYTE* p = nullptr; UINT32 n = 0; DWORD flags = 0;
                    if (FAILED(cap->GetBuffer(&p, &n, &flags, nullptr, nullptr)))
                        break;
                    frames += n;
                    cap->ReleaseBuffer(n);
                }
                ac->Stop();
                cap->Release();
                // Started cleanly and delivered nothing: that is the wedge, and
                // it is the only conclusion this function is entitled to draw.
                // Every failure above leaves Unknown, so "could not tell" is
                // never reported to the user as "broken".
                result = frames > 0 ? micwatch::Health::Alive : micwatch::Health::Wedged;
            }
        }
        if (wfx) CoTaskMemFree(wfx);
        if (ac)  ac->Release();
        dev->Release();
        return result;
    }
}

namespace micwatch
{

void CheckPs3Async(HWND wnd, UINT msg)
{
    if (g_reported.load(std::memory_order_acquire))
        return;                                    // already told them this plug
    bool expected = false;
    if (!g_inFlight.compare_exchange_strong(expected, true))
        return;                                    // a probe is already running

    // The previous probe has finished its work (g_inFlight was clear) but its
    // thread object still needs reaping before the slot can be reused.
    if (g_probe.joinable())
        g_probe.join();

    g_probe = std::thread([wnd, msg] {
        // Own apartment: this runs off the UI thread and must not disturb its
        // COM state.
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        const Health h = com ? ProbePs3Mic() : Health::Unknown;
        if (com) CoUninitialize();

        g_last.store(static_cast<int>(h), std::memory_order_release);
        if (h != Health::Unknown)
            g_reported.store(true, std::memory_order_release);
        g_inFlight.store(false, std::memory_order_release);

        if (h == Health::Wedged)
            HostLog(L"PS3 Eye microphone delivers no audio -- needs a physical replug");
        if (wnd)
            PostMessageW(wnd, msg, static_cast<WPARAM>(h), 0);
    });
}

void Shutdown()
{
    if (g_probe.joinable())
        g_probe.join();
}

Health LastPs3Result()
{
    return static_cast<Health>(g_last.load(std::memory_order_acquire));
}

void Reset()
{
    g_reported.store(false, std::memory_order_release);
    g_last.store(static_cast<int>(Health::Unknown), std::memory_order_release);
}

} // namespace micwatch
