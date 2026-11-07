#include "MicWatch.h"

#include <initguid.h>   // DEFINE the PROPERTYKEYs used below, don't just declare
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wrl/client.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "HostLog.h"

#pragma comment(lib, "cfgmgr32.lib")

using Microsoft::WRL::ComPtr;

namespace
{
    // The PS3 Eye's whole-device and audio-function instance ID prefixes. The
    // camera is a composite device: usbccgp splits it into MI_00 (video, ours,
    // WinUSB) and MI_01 (audio, the inbox usbaudio class driver).
    constexpr wchar_t kPs3Prefix[]  = L"USB\\VID_1415&PID_2000";
    constexpr wchar_t kPs3AudioIf[] = L"&MI_01\\";

    // The PS3 Eye's audio function names itself this. Matching on the fragment
    // rather than the whole string survives Windows' "(2- ...)" disambiguation
    // prefix, which appears whenever the camera has been seen on two ports.
    constexpr wchar_t kPs3MicName[] = L"B4.09.24.1";

    std::mutex   g_detailLock;
    std::wstring g_detail;      // service name / problem code behind the last result

    std::atomic<int>  g_last{ static_cast<int>(micwatch::Health::Unknown) };
    std::atomic<bool> g_inFlight{ false };
    std::atomic<bool> g_reported{ false };

    // The probe thread is OWNED, not detached. It holds COM and runs for up to
    // 1.2 s, so a detached one can still be inside WASAPI while the process
    // tears its runtime down. At most one exists at a time (g_inFlight), and
    // Shutdown joins it. UI thread only.
    std::thread g_probe;

    // The first PS3 Eye capture endpoint in `state` (a DEVICE_STATE_* mask), or
    // null when there is none.
    ComPtr<IMMDevice> FindPs3Endpoint(DWORD state)
    {
        ComPtr<IMMDeviceEnumerator> en;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(&en))) || !en)
            return nullptr;

        ComPtr<IMMDeviceCollection> col;
        if (FAILED(en->EnumAudioEndpoints(eCapture, state, &col)) || !col)
            return nullptr;
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n; ++i)
        {
            ComPtr<IMMDevice> d;
            if (FAILED(col->Item(i, &d)) || !d)
                continue;
            ComPtr<IPropertyStore> ps;
            bool match = false;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps)) && ps)
            {
                PROPVARIANT pv;
                PropVariantInit(&pv);
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR)
                    match = wcsstr(pv.pwszVal, kPs3MicName) != nullptr;
                PropVariantClear(&pv);
            }
            if (match)
                return d;
        }
        return nullptr;
    }

    // ACTIVE only: a camera that has been plugged into several ports leaves
    // unplugged endpoint records behind, and one of those would answer for a
    // device that is not there.
    ComPtr<IMMDevice> FindPs3Mic() { return FindPs3Endpoint(DEVICE_STATE_ACTIVE); }

    bool HasDisabledPs3Endpoint() { return FindPs3Endpoint(DEVICE_STATE_DISABLED).Get() != nullptr; }

    // A PS3 Eye with no ACTIVE capture endpoint. Says why, when the reason is
    // one the user can act on; Unknown otherwise (no camera plugged, or the
    // audio driver is still building the endpoint).
    //
    // The common cause is a driver bound to the WHOLE device (Zadig with
    // "ignore composite parents" unticked, CL-Eye, PSMoveService, or any
    // VID/PID-only WinUSB INF). usbccgp then never loads, so the audio
    // function never exists, while capture still works -- libusb opens a
    // whole-device WinUSB/libusbK binding just as happily as our MI_00 one.
    micwatch::Health DiagnoseMissingEndpoint(std::wstring& detail)
    {
        HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"USB", nullptr,
                                            DIGCF_ALLCLASSES | DIGCF_PRESENT);
        if (set == INVALID_HANDLE_VALUE)
            return micwatch::Health::Unknown;

        const size_t prefixLen = wcslen(kPs3Prefix);
        bool parentOk = false, foreign = false, audioOk = false, audioProblem = false;
        std::wstring foreignService;
        ULONG problemCode = 0;

        SP_DEVINFO_DATA d{ sizeof(d) };
        for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i)
        {
            wchar_t id[MAX_DEVICE_ID_LEN] = {};
            if (CM_Get_Device_IDW(d.DevInst, id, MAX_DEVICE_ID_LEN, 0) != CR_SUCCESS ||
                _wcsnicmp(id, kPs3Prefix, prefixLen) != 0)
                continue;
            const wchar_t* rest = id + prefixLen;
            if (*rest == L'\\')                       // the composite parent itself
            {
                wchar_t svc[64] = {};
                DWORD type = 0;
                if (!SetupDiGetDeviceRegistryPropertyW(set, &d, SPDRP_SERVICE, &type,
                                                       (BYTE*)svc, sizeof(svc) - sizeof(wchar_t), nullptr))
                    svc[0] = 0;
                if (_wcsicmp(svc, L"usbccgp") == 0)
                    parentOk = true;
                else
                {
                    foreign = true;
                    foreignService = svc[0] ? svc : L"no driver";
                }
            }
            else if (_wcsnicmp(rest, kPs3AudioIf, wcslen(kPs3AudioIf)) == 0)
            {
                ULONG status = 0, problem = 0;
                if (CM_Get_DevNode_Status(&status, &problem, d.DevInst, 0) == CR_SUCCESS &&
                    (status & DN_HAS_PROBLEM))
                {
                    audioProblem = true;
                    problemCode = problem;
                }
                else
                    audioOk = true;
            }
        }
        SetupDiDestroyDeviceInfoList(set);

        if (foreign)
        {
            detail = foreignService;
            return micwatch::Health::WholeDeviceDriver;
        }
        if (audioProblem)
        {
            detail = L"Code " + std::to_wstring(problemCode);
            return micwatch::Health::AudioDriverError;
        }
        if (parentOk && audioOk && HasDisabledPs3Endpoint())
            return micwatch::Health::EndpointDisabled;
        return micwatch::Health::Unknown;
    }

    micwatch::Health ProbePs3Mic(std::wstring& detail)
    {
        const ComPtr<IMMDevice> dev = FindPs3Mic();
        if (!dev)
            return DiagnoseMissingEndpoint(detail);

        ComPtr<IAudioClient> ac;
        micwatch::Health result = micwatch::Health::Unknown;

        // SHARED deliberately: exclusive would evict whatever app is using the
        // mic, and this is a background health check, not a recording. A wedged
        // array reads empty in shared mode too -- that was measured.
        WAVEFORMATEX* wfx = nullptr;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(ac.GetAddressOf()))) && ac &&
            SUCCEEDED(ac->GetMixFormat(&wfx)) && wfx &&
            SUCCEEDED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10'000'000, 0, wfx, nullptr)))
        {
            ComPtr<IAudioCaptureClient> cap;
            if (SUCCEEDED(ac->GetService(IID_PPV_ARGS(&cap))) && cap)
            {
                bool apiFailed = FAILED(ac->Start());
                long long frames = 0;
                // 1.2 s: measured over 1.5 s windows, a healthy array never took
                // more than a few tens of ms to produce its first packet, and a
                // wedged one produces nothing however long you wait.
                const ULONGLONG until = GetTickCount64() + 1200;
                while (!apiFailed && GetTickCount64() < until && frames == 0)
                {
                    UINT32 avail = 0;
                    if (FAILED(cap->GetNextPacketSize(&avail)))
                    { apiFailed = true; break; }
                    if (!avail) { Sleep(5); continue; }
                    BYTE* p = nullptr; UINT32 n = 0; DWORD flags = 0;
                    if (FAILED(cap->GetBuffer(&p, &n, &flags, nullptr, nullptr)))
                    { apiFailed = true; break; }
                    frames += n;
                    if (FAILED(cap->ReleaseBuffer(n))) apiFailed = true;
                }
                ac->Stop();
                // Started cleanly and delivered nothing: that is the wedge, and
                // it is the only conclusion this function is entitled to draw.
                // Every failure above leaves Unknown, so "could not tell" is
                // never reported to the user as "broken".
                result = apiFailed ? micwatch::Health::Unknown
                                   : (frames > 0 ? micwatch::Health::Alive : micwatch::Health::Wedged);
            }
        }
        if (wfx) CoTaskMemFree(wfx);
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
        std::wstring detail;
        const Health h = com ? ProbePs3Mic(detail) : Health::Unknown;
        if (com) CoUninitialize();

        {
            std::lock_guard<std::mutex> lk(g_detailLock);
            g_detail = detail;
        }
        g_last.store(static_cast<int>(h), std::memory_order_release);
        if (h != Health::Unknown)
            g_reported.store(true, std::memory_order_release);
        g_inFlight.store(false, std::memory_order_release);

        if (h == Health::Wedged)
            HostLog(L"PS3 Eye microphone delivers no audio -- needs a physical replug");
        else if (h == Health::WholeDeviceDriver)
            HostLog(L"PS3 Eye is bound as a whole device (%s) -- no audio function", detail.c_str());
        else if (h == Health::AudioDriverError)
            HostLog(L"PS3 Eye audio function has a driver problem (%s)", detail.c_str());
        else if (h == Health::EndpointDisabled)
            HostLog(L"PS3 Eye microphone endpoint is disabled in Sound settings");
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

std::wstring LastPs3Detail()
{
    std::lock_guard<std::mutex> lk(g_detailLock);
    return g_detail;
}

void Reset()
{
    g_reported.store(false, std::memory_order_release);
    g_last.store(static_cast<int>(Health::Unknown), std::memory_order_release);
}

} // namespace micwatch
