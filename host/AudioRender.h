#pragma once
//
// AudioRender — plays the PS4 microphone into a Windows OUTPUT device.
//
// This is how the array gets out of the tray. Windows has no user-space way to
// create a recording endpoint (every virtual mic is a kernel driver), but it has
// always let us WRITE to one — so we render into whatever output the user picks.
// Point it at VB-CABLE / Voicemeeter / Steam Streaming Microphone / NVIDIA
// Broadcast and the array becomes a real microphone in every app, with no driver
// from us. Point it at headphones and it is a monitor.
//
// The reason this is a QUALITY feature and not a convenience one: what actually
// limits this array is far-field room noise, and the fix for that is noise
// suppression — which Discord, Teams, OBS and Windows all ship, better than we
// would write, and none of which can reach audio that only exists inside our
// process.
//
// ---- the two hard parts ---------------------------------------------------
//
// 1. NEVER BLOCK THE CAPTURE THREAD. Push() is wait-free: it writes into a ring
//    and returns. The render thread owns the endpoint and does all the waiting.
//
// 2. THE CLOCKS DO NOT MATCH, and not by a little. The camera runs at 60.029 fps
//    rather than 60.000, so the microphone really arrives at ~48,023 Hz while a
//    48,000 Hz endpoint consumes 48,000 — **+23 samples a second, which overruns
//    a 40 ms buffer in under two seconds**. The offset is also
//    per video mode: the two 640x400 modes share a line rate 533 ppm away from
//    1280x800@60, so it cannot be tabulated either.
//
//    So the read ratio is closed-loop on RING FILL, not open-loop on any
//    measured constant. Fill is the only observable that sees both clocks —
//    ours and the endpoint's, which drifts too. AudioMeasuredRateMilliHz() seeds
//    the loop so it starts near the answer; it is not trusted to stay there.
//
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace audiorender {

struct Endpoint
{
    std::wstring id;        // MMDevice id, stable across reboots — persist THIS
    std::wstring name;      // friendly name, for the dropdown
    bool isDefault = false;
    // True for endpoints whose name matches a known virtual-cable product, so
    // the UI can float them to the top: pointing at one of these is the whole
    // "use the PS4 mic in Discord" story, and users will not guess that.
    bool looksLikeVirtualCable = false;
    // When it IS a cable: the capture endpoint audio sent here comes back out
    // of — i.e. the name the user must pick in Discord. Verified present, not
    // inferred from the render name (Steam Streaming Speakers fooled that).
    std::wstring captureName;
};

enum class Downmix { Mono = 0, StereoFrontPair = 1 };

struct Status
{
    bool     active        = false;
    uint32_t fillMs        = 0;      // current ring occupancy
    int32_t  correctionPpm = 0;      // what the drift loop settled on
    uint64_t underruns     = 0;      // ring went empty: audible gaps
    uint64_t overruns      = 0;      // ring went full: we discarded audio
    uint32_t endpointRate  = 0;
    uint32_t endpointChans = 0;
};

// Enumerate active render endpoints. Safe to call from the UI thread.
std::vector<Endpoint> ListEndpoints();

class Renderer
{
public:
    Renderer();
    ~Renderer();
    Renderer(const Renderer&) = delete;
    Renderer& operator=(const Renderer&) = delete;

    // Open `deviceId` and start rendering. Empty id stops. Idempotent: calling
    // with the id already running is a no-op, so the capture loop can call it
    // every frame without churn.
    bool Start(const std::wstring& deviceId, Downmix mix);

    // Release the endpoint entirely. Software that squats on an audio device
    // while idle is justly disliked, so nothing holds it when disabled.
    void Stop();

    bool Running() const { return _running.load(std::memory_order_acquire); }

    // Feed captured audio. WAIT-FREE — called from the camera thread.
    // `frames` interleaved sample frames of `channels` int16 each.
    // `srcRateMilliHz` is AudioMeasuredRateMilliHz(), 0 if not yet known.
    void Push(const int16_t* src, uint32_t frames, uint32_t channels,
              uint32_t srcRateMilliHz);

    Status GetStatus() const;

private:
    struct Impl;
    std::unique_ptr<Impl> _impl;
    std::atomic<bool> _running{ false };
};

} // namespace audiorender
