//
// AudioRender — see AudioRender.h for what this is and why the drift loop is
// mandatory rather than a refinement.
//
#include "AudioRender.h"

#include <windows.h>
#include <initguid.h>            // DEFINE the PROPERTYKEYs, don't just declare
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <thread>

#pragma comment(lib, "avrt.lib")

namespace audiorender {

static const GUID kSubPcm =
    { 0x00000001, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } };
static const GUID kSubFloat =
    { 0x00000003, 0x0000, 0x0010, { 0x80,0x00,0x00,0xaa,0x00,0x38,0x9b,0x71 } };

// Is this render endpoint one half of a LOOPBACK CABLE — i.e. will audio sent
// here come back out of some capture device that other apps can select?
//
// This is the question the user actually has ("will Discord see my mic?"), and
// name-matching alone answers it wrongly. A first pass flagged "Steam Streaming
// Speakers", which is Remote Play's output, not a loopback: nothing local
// captures it. So a render endpoint is only called a cable when its PARTNER
// CAPTURE ENDPOINT IS ACTUALLY PRESENT on this machine.
//
// Pairs are matched by product marker rather than exact name because vendors
// decorate these with driver revisions and numbering.
struct CablePair { const wchar_t* renderMark; const wchar_t* captureMark; };
static const CablePair kCablePairs[] = {
    { L"CABLE Input",          L"CABLE Output"          },   // VB-CABLE
    { L"CABLE-A Input",        L"CABLE-A Output"        },   // VB-CABLE A+B
    { L"CABLE-B Input",        L"CABLE-B Output"        },
    { L"VoiceMeeter Input",    L"VoiceMeeter Output"    },
    { L"VoiceMeeter Aux Input",L"VoiceMeeter Aux Output"},
    { L"VoiceMeeter VAIO3",    L"VoiceMeeter VAIO3"     },
    { L"Line 1 (Virtual Audio Cable)", L"Line 1 (Virtual Audio Cable)" },  // VAC
    { L"Virtual Audio Cable",  L"Virtual Audio Cable"   },
};

static bool ContainsI(const std::wstring& hay, const wchar_t* needle)
{
    const size_t nl = wcslen(needle);
    if (nl > hay.size()) return false;
    for (size_t i = 0; i + nl <= hay.size(); ++i)
        if (_wcsnicmp(hay.c_str() + i, needle, nl) == 0) return true;
    return false;
}

static std::wstring FriendlyName(IMMDevice* d)
{
    IPropertyStore* ps = nullptr;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps)) || !ps) return L"";
    PROPVARIANT pv; PropVariantInit(&pv);
    std::wstring out;
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
        out = pv.pwszVal;
    PropVariantClear(&pv);
    ps->Release();
    return out;
}

// Friendly names of every active CAPTURE endpoint, so a render endpoint can be
// checked for a live partner rather than guessed at from its name.
static std::vector<std::wstring> CaptureNames(IMMDeviceEnumerator* en)
{
    std::vector<std::wstring> out;
    IMMDeviceCollection* col = nullptr;
    if (SUCCEEDED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col)) && col)
    {
        UINT n = 0; col->GetCount(&n);
        for (UINT i = 0; i < n; ++i)
        {
            IMMDevice* d = nullptr;
            if (SUCCEEDED(col->Item(i, &d)) && d) { out.push_back(FriendlyName(d)); d->Release(); }
        }
        col->Release();
    }
    return out;
}

std::vector<Endpoint> ListEndpoints()
{
    std::vector<Endpoint> out;
    // The caller may or may not have COM up; ask for it and tolerate either.
    // S_FALSE means "already initialized on this thread" and STILL owes a
    // CoUninitialize — the count is per successful call, not per thread — so
    // anything that SUCCEEDED must be balanced below.
    const bool weInit = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));

    IMMDeviceEnumerator* en = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   __uuidof(IMMDeviceEnumerator), (void**)&en)) && en)
    {
        std::wstring defaultId;
        IMMDevice* def = nullptr;
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &def)) && def)
        {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id)) && id) { defaultId = id; CoTaskMemFree(id); }
            def->Release();
        }
        const std::vector<std::wstring> captures = CaptureNames(en);
        IMMDeviceCollection* col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col)) && col)
        {
            UINT n = 0; col->GetCount(&n);
            for (UINT i = 0; i < n; ++i)
            {
                IMMDevice* d = nullptr;
                if (FAILED(col->Item(i, &d)) || !d) continue;
                LPWSTR id = nullptr;
                if (SUCCEEDED(d->GetId(&id)) && id)
                {
                    Endpoint e;
                    e.id = id;
                    e.name = FriendlyName(d);
                    e.isDefault = (e.id == defaultId);
                    for (const auto& pr : kCablePairs)
                        if (ContainsI(e.name, pr.renderMark))
                            for (const auto& cn : captures)
                                if (ContainsI(cn, pr.captureMark))
                                { e.looksLikeVirtualCable = true; e.captureName = cn; break; }
                    out.push_back(std::move(e));
                    CoTaskMemFree(id);
                }
                d->Release();
            }
            col->Release();
        }
        en->Release();
    }
    if (weInit) CoUninitialize();

    // Cables first, then default, then the rest — the ordering the UI wants.
    std::stable_sort(out.begin(), out.end(), [](const Endpoint& a, const Endpoint& b) {
        if (a.looksLikeVirtualCable != b.looksLikeVirtualCable) return a.looksLikeVirtualCable;
        return a.isDefault && !b.isDefault;
    });
    return out;
}

// ---------------------------------------------------------------------------

struct Renderer::Impl
{
    // ---- ring, capture thread writes / render thread reads ----------------
    // Half a second is far more than the working depth; the headroom exists so
    // a scheduling hiccup on either side costs latency rather than a dropout.
    static const uint32_t kRingFrames = 24000;      // 0.5 s at 48 kHz
    static const uint32_t kMaxSrcCh   = 4;
    int16_t  ring[kRingFrames * 2] = {};            // up to stereo
    uint32_t ringCh = 1;
    std::atomic<uint64_t> wpos{ 0 }, rpos{ 0 };

    // ---- drift loop -------------------------------------------------------
    // Target depth. Big enough to absorb the camera's 60 Hz burst cadence (a
    // whole video frame of audio arrives at once), small enough to stay a
    // sensible monitoring latency.
    static const uint32_t kTargetMs = 40;
    double   phase = 0.0;             // fractional read position within the ring
    double   ratio = 1.0;             // read step; 1.0 = source and sink agree
    double   prev[2] = {};            // last emitted, for the interpolator tail

    std::atomic<uint32_t> srcRateMilliHz{ 0 };
    std::atomic<uint32_t> fillMs{ 0 };
    std::atomic<int32_t>  correctionPpm{ 0 };
    std::atomic<uint64_t> underruns{ 0 }, overruns{ 0 };
    std::atomic<uint32_t> endpointRate{ 0 }, endpointChans{ 0 };

    std::wstring deviceId;
    Downmix      mix = Downmix::Mono;

    std::thread  thread;
    std::atomic<bool> stop{ false };
    std::mutex   startMutex;

    void Reset()
    {
        wpos.store(0); rpos.store(0);
        phase = 0.0; ratio = 1.0;
        prev[0] = prev[1] = 0.0;
        fillMs.store(0); correctionPpm.store(0);
        underruns.store(0); overruns.store(0);
    }

    uint32_t Fill() const
    {
        const uint64_t w = wpos.load(std::memory_order_acquire);
        const uint64_t r = rpos.load(std::memory_order_acquire);
        return (uint32_t)(w > r ? std::min<uint64_t>(w - r, kRingFrames) : 0);
    }

    void Run();
};

Renderer::Renderer() : _impl(new Impl) {}
Renderer::~Renderer() { Stop(); }

bool Renderer::Start(const std::wstring& deviceId, Downmix mix)
{
    std::lock_guard<std::mutex> lk(_impl->startMutex);
    if (_running.load(std::memory_order_acquire) &&
        _impl->deviceId == deviceId && _impl->mix == mix)
        return true;                                   // already what was asked

    // stop whatever is running before re-opening
    if (_impl->thread.joinable())
    {
        _impl->stop.store(true, std::memory_order_release);
        _impl->thread.join();
    }
    _running.store(false, std::memory_order_release);
    if (deviceId.empty()) { _impl->deviceId.clear(); return true; }

    _impl->deviceId = deviceId;
    _impl->mix = mix;
    _impl->ringCh = (mix == Downmix::StereoFrontPair) ? 2 : 1;
    _impl->Reset();
    _impl->stop.store(false, std::memory_order_release);
    _running.store(true, std::memory_order_release);
    _impl->thread = std::thread([this] { _impl->Run(); _running.store(false, std::memory_order_release); });
    return true;
}

void Renderer::Stop()
{
    std::lock_guard<std::mutex> lk(_impl->startMutex);
    if (_impl->thread.joinable())
    {
        _impl->stop.store(true, std::memory_order_release);
        _impl->thread.join();
    }
    _running.store(false, std::memory_order_release);
    _impl->deviceId.clear();
}

// Wait-free. Downmix and write; drop on overrun rather than block, because the
// alternative is stalling the camera thread on an audio device.
void Renderer::Push(const int16_t* src, uint32_t frames, uint32_t channels,
                    uint32_t srcRateMilliHz)
{
    if (!_running.load(std::memory_order_acquire) || !src || !frames || !channels)
        return;
    Impl& m = *_impl;
    if (srcRateMilliHz) m.srcRateMilliHz.store(srcRateMilliHz, std::memory_order_relaxed);

    const uint32_t oc = m.ringCh;
    uint64_t w = m.wpos.load(std::memory_order_relaxed);
    const uint64_t r = m.rpos.load(std::memory_order_acquire);
    uint32_t space = (uint32_t)(Impl::kRingFrames - std::min<uint64_t>(w - r, Impl::kRingFrames));
    if (frames > space)
    {
        m.overruns.fetch_add(1, std::memory_order_relaxed);
        frames = space;                          // keep the oldest; drop the tail
        if (!frames) return;
    }
    for (uint32_t i = 0; i < frames; ++i)
    {
        const int16_t* f = src + (size_t)i * channels;
        const size_t slot = (size_t)(w % Impl::kRingFrames) * oc;
        if (oc == 1)
        {
            // Average the capsules. Measured +2.46 dB, and voice correlates
            // ~0.99 across them so the average costs the signal nothing.
            int32_t s = 0;
            for (uint32_t c = 0; c < channels; ++c) s += f[c];
            m.ring[slot] = (int16_t)(s / (int32_t)channels);
        }
        else
        {
            m.ring[slot]     = f[0];
            m.ring[slot + 1] = f[channels > 1 ? 1 : 0];
        }
        ++w;
    }
    m.wpos.store(w, std::memory_order_release);
}

Status Renderer::GetStatus() const
{
    Status s;
    s.active        = _running.load(std::memory_order_acquire);
    s.fillMs        = _impl->fillMs.load(std::memory_order_relaxed);
    s.correctionPpm = _impl->correctionPpm.load(std::memory_order_relaxed);
    s.underruns     = _impl->underruns.load(std::memory_order_relaxed);
    s.overruns      = _impl->overruns.load(std::memory_order_relaxed);
    s.endpointRate  = _impl->endpointRate.load(std::memory_order_relaxed);
    s.endpointChans = _impl->endpointChans.load(std::memory_order_relaxed);
    return s;
}

// ---------------------------------------------------------------------------

void Renderer::Impl::Run()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* en = nullptr;
    IMMDevice*           dev = nullptr;
    IAudioClient*        ac = nullptr;
    IAudioRenderClient*  rc = nullptr;
    WAVEFORMATEX*        wfx = nullptr;
    HANDLE               evt = nullptr;
    HANDLE               mmcss = nullptr;

    auto cleanup = [&] {
        if (ac) ac->Stop();
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        if (rc) rc->Release();
        if (ac) ac->Release();
        if (wfx) CoTaskMemFree(wfx);
        if (evt) CloseHandle(evt);
        if (dev) dev->Release();
        if (en) en->Release();
        CoUninitialize();
    };

    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), (void**)&en)) || !en)
    { cleanup(); return; }
    if (FAILED(en->GetDevice(deviceId.c_str(), &dev)) || !dev) { cleanup(); return; }
    if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void**)&ac)) || !ac)
    { cleanup(); return; }
    if (FAILED(ac->GetMixFormat(&wfx)) || !wfx) { cleanup(); return; }

    bool sinkFloat = wfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (wfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE)
    {
        auto* ex = (WAVEFORMATEXTENSIBLE*)wfx;
        sinkFloat = IsEqualGUID(ex->SubFormat, kSubFloat) != 0;
        if (!sinkFloat && !IsEqualGUID(ex->SubFormat, kSubPcm)) { cleanup(); return; }
    }
    const uint32_t sinkCh   = wfx->nChannels;
    const uint32_t sinkRate = wfx->nSamplesPerSec;
    const uint32_t sinkBytes = wfx->wBitsPerSample / 8;

    // The fan-out at the bottom of this function writes float32, int16 or
    // int32 and nothing else. At any other sample width it writes NOTHING —
    // and ReleaseBuffer would then hand the untouched, uninitialized buffer to
    // the endpoint as audio. Refuse the device instead: silence from an
    // unsupported format is a bug report, noise is a burst of garbage in
    // someone's headphones.
    if (sinkFloat ? sinkBytes != 4 : (sinkBytes != 2 && sinkBytes != 4))
    { cleanup(); return; }

    endpointRate.store(sinkRate, std::memory_order_relaxed);
    endpointChans.store(sinkCh, std::memory_order_relaxed);

    // Shared, event-driven, ~40 ms. Shared mode so we coexist with everything
    // else; event-driven so the thread sleeps rather than polls.
    const REFERENCE_TIME dur = 400000;                    // 40 ms in 100 ns units
    HRESULT hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_EVENTCALLBACK, dur, 0, wfx, nullptr);
    if (FAILED(hr)) { cleanup(); return; }
    evt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!evt || FAILED(ac->SetEventHandle(evt))) { cleanup(); return; }
    UINT32 bufFrames = 0;
    if (FAILED(ac->GetBufferSize(&bufFrames))) { cleanup(); return; }
    if (FAILED(ac->GetService(__uuidof(IAudioRenderClient), (void**)&rc)) || !rc)
    { cleanup(); return; }

    DWORD taskIndex = 0;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    // Nominal read step for the camera's measured rate against this endpoint's.
    // Re-read per iteration rather than snapshotted once: routing is often
    // enabled before the first video frame exists, and a rate of 0 at thread
    // start would otherwise pin the loop's operating point at 1.0 for its whole
    // life, leaving the camera's real +483 ppm to be carried as a standing fill
    // offset by the proportional term alone.
    auto nominalRatio = [&]() -> double {
        const uint32_t r = srcRateMilliHz.load(std::memory_order_relaxed);
        const double n = r ? (double)r / 1000.0 / (double)sinkRate : 1.0;
        return (n < 0.9 || n > 1.1) ? 1.0 : n;
    };
    ratio = nominalRatio();

    const uint32_t targetFrames = sinkRate * kTargetMs / 1000;

    // Prime: hand the engine silence until we have a working depth, otherwise
    // the first callbacks underrun before the camera has produced anything.
    ac->Start();
    bool primed = false;

    while (!stop.load(std::memory_order_acquire))
    {
        if (WaitForSingleObject(evt, 200) != WAIT_OBJECT_0)
            continue;

        UINT32 padding = 0;
        if (FAILED(ac->GetCurrentPadding(&padding))) break;
        const UINT32 want = bufFrames > padding ? bufFrames - padding : 0;
        if (!want) continue;

        BYTE* out = nullptr;
        if (FAILED(rc->GetBuffer(want, &out)) || !out) continue;

        const uint32_t avail = Fill();
        fillMs.store(sinkRate ? avail * 1000 / sinkRate : 0, std::memory_order_relaxed);

        if (!primed)
        {
            if (avail < targetFrames)
            {
                rc->ReleaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT);
                continue;
            }
            primed = true;
            phase = 0.0;
        }

        // ---- drift loop --------------------------------------------------
        // Proportional on FILL, which is an integrator of rate error, so this
        // is textbook-stable without an explicit integral term. Gentle gain and
        // a hard clamp: the correction we need is hundreds of ppm, so anything
        // approaching a percent means something else is wrong and we would
        // rather glide than chase it.
        {
            const double err = ((double)avail - (double)targetFrames) / (double)targetFrames;
            const double kp  = 0.02;
            double target = nominalRatio() * (1.0 + kp * err);
            if (target < 0.97) target = 0.97;
            if (target > 1.03) target = 1.03;
            ratio += (target - ratio) * 0.05;          // slew, so it never steps
            correctionPpm.store((int32_t)((ratio - 1.0) * 1e6), std::memory_order_relaxed);
        }

        // ---- resample ring -> endpoint ------------------------------------
        const uint32_t oc = ringCh;
        uint64_t r = rpos.load(std::memory_order_relaxed);
        const uint64_t w = wpos.load(std::memory_order_acquire);
        bool starved = false;

        for (UINT32 i = 0; i < want; ++i)
        {
            double s[2] = { prev[0], prev[1] };
            if (r + 1 < w)
            {
                const size_t a = (size_t)(r % kRingFrames) * oc;
                const size_t b = (size_t)((r + 1) % kRingFrames) * oc;
                const double t = phase;
                for (uint32_t c = 0; c < oc; ++c)
                    s[c] = ring[a + c] * (1.0 - t) + ring[b + c] * t;   // linear is
                                                                        // ample at
                                                                        // ratio ~1
                prev[0] = s[0]; prev[1] = s[1];
                phase += ratio;
                while (phase >= 1.0) { phase -= 1.0; ++r; }
            }
            else
                starved = true;    // hold the last sample rather than click

            // fan out to the endpoint's channel count
            BYTE* dst = out + (size_t)i * sinkCh * sinkBytes;
            for (uint32_t c = 0; c < sinkCh; ++c)
            {
                const double v = s[(oc == 2 && c < 2) ? c : 0] / 32768.0;
                if (sinkFloat) *(float*)(dst + (size_t)c * sinkBytes) = (float)v;
                else if (sinkBytes == 2)
                {
                    int32_t q = (int32_t)(v * 32768.0);
                    q = q > 32767 ? 32767 : (q < -32768 ? -32768 : q);
                    *(int16_t*)(dst + (size_t)c * sinkBytes) = (int16_t)q;
                }
                else if (sinkBytes == 4)
                {
                    double q = v * 2147483648.0;
                    if (q > 2147483647.0) q = 2147483647.0;
                    if (q < -2147483648.0) q = -2147483648.0;
                    *(int32_t*)(dst + (size_t)c * sinkBytes) = (int32_t)q;
                }
            }
        }
        rpos.store(r, std::memory_order_release);
        if (starved) underruns.fetch_add(1, std::memory_order_relaxed);
        rc->ReleaseBuffer(want, 0);
    }
    cleanup();
}

} // namespace audiorender
