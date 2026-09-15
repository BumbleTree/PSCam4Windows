#pragma once
//
// Ps4ViewDevice — an ICameraDevice that presents ONE logical view (Left, Right,
// or SideBySide) of a shared Ps4CaptureSource as a virtual camera.
//
// It is a thin crop + demosaic adapter: it holds a shared_ptr to the per-camera
// Ps4CaptureSource (so split mode = two views over one source) and a Ps4ViewKind.
// AcquireFrame pulls the newest raw-Bayer eye plane(s) off the source's latch and
// runs the native-quality GRBG demosaic (Ps4Demosaic) into its YUY2 output. The
// CaptureController drives it like any other device and stays the sole FrameBus
// writer for its slot — no PS4 special-casing leaks up the stack.
//
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "../../host/ICameraDevice.h"
#include "Ps4Demosaic.h"

namespace ps4 { class Ps4CaptureSource; }

class Ps4ViewDevice final : public ICameraDevice
{
public:
    // portPath selects the physical PS4 (registry slot key; empty = first found);
    // view selects which eye/stitch this virtual camera presents.
    // ownsControls=false for a split pair's secondary (Right) half: both halves
    // share ONE physical OV580 ISP, so only the home (Left/switchable) view drives
    // its exposure/gain/brightness — otherwise the two capture threads would fight
    // over the single shared control state.
    Ps4ViewDevice(std::string portPath, Ps4ViewKind view, bool ownsControls = true)
        : _portPath(std::move(portPath)), _view(view), _ownsControls(ownsControls) {}
    ~Ps4ViewDevice() override;

    bool Init(const VideoMode& mode) override;
    bool Start() override;
    void Stop() override;
    bool AcquireFrame(AcquiredFrame& out, uint32_t timeoutMs, bool wantJpeg) override;
    void ApplySettings(const Settings& s) override;
    const DeviceProfile& Profile() const override;
    bool SetView(int viewKind) override;

    // What this view is really producing: the shared engine's committed
    // geometry, which is not necessarily what this slot asked for (a split
    // pair's non-owning half adopts the owner's mode).
    VideoMode ActualMode(const VideoMode& requested) const override
    { (void)requested; return VideoMode{ _width, _height, _fps }; }

    // Microphone: one array per physical camera, so — like the shared ISP
    // controls — only the view that owns them exposes it. A split pair's other
    // half reports no audio rather than both halves draining the same ring.
    uint32_t AudioChannels() const override;
    uint32_t AudioSampleRate() const override;
    uint32_t AudioMeasuredRateMilliHz() const override;
    uint32_t ReadAudio(int16_t* dst, uint32_t frames) override;
    bool AudioLevels(float* out, uint32_t count) override;
    uint32_t AudioDropouts() const override;   // live Left/Right/SBS, no stream restart

private:
    std::string _portPath;
    Ps4ViewKind _view;
    bool        _ownsControls = true;   // false = split secondary (Right) half

    std::shared_ptr<ps4::Ps4CaptureSource> _source;
    bool     _streaming = false;
    bool     _useIsp = false;   // source is YUYV ISP -> planes are YUY2, copy not demosaic
    uint32_t _width = 0, _height = 0, _fps = 0;
    uint64_t _lastSeq = 0;

    // Software flip (latched by ApplySettings, consumed by the demosaic; both on
    // the capture thread, so no sync needed).
    bool _flipH = false, _flipV = false;

    // Per-eye tone state (persistent auto-WB/level smoothing). SideBySide tones
    // each eye independently so they don't fight each other.
    ps4::Ps4ToneState _toneL, _toneR;

    // Capture-thread scratch: raw Bayer plane(s) snapshotted from the source, and
    // (SideBySide only) per-eye YUY2 scratch composited into the wide output.
    std::vector<uint8_t> _bayerL, _bayerR;
    std::vector<uint8_t> _eyeYuyvL, _eyeYuyvR;
    std::unique_ptr<uint8_t[]> _yuy2Out;   // published YUY2 (width*height*2)
};
