#include "Ps4ViewDevice.h"

#include <cstring>
#include <new>

#include "Ps4CaptureSource.h"
#include "../../host/DeviceProfiles.h"
#include "../../common/FrameBus.h"   // framebus::Yuy2Bytes
#include "../../common/Yuv.h"

namespace {
// Copy a w x h YUY2 plane to dst with optional flips. No flip is a plain memcpy
// (the hot path); flipV reverses row order; flipH reverses the macropixel order
// within each row, swapping Y0/Y1 so the YUY2 packing stays valid.
void Yuy2View(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h, bool flipH, bool flipV)
{
    const size_t rowBytes = (size_t)w * 2;
    for (uint32_t y = 0; y < h; ++y)
    {
        const uint8_t* s = src + (size_t)(flipV ? (h - 1 - y) : y) * rowBytes;
        uint8_t*       o = dst + (size_t)y * rowBytes;
        if (!flipH) { memcpy(o, s, rowBytes); continue; }
        const uint32_t macros = w / 2;                 // each = Y0 U Y1 V
        for (uint32_t j = 0; j < macros; ++j)
        {
            const uint8_t* sm = s + (size_t)(macros - 1 - j) * 4;
            uint8_t*       om = o + (size_t)j * 4;
            om[0] = sm[2];  om[1] = sm[1];  om[2] = sm[0];  om[3] = sm[3];
        }
    }
}
} // namespace

Ps4ViewDevice::~Ps4ViewDevice()
{
    Stop();
}

bool Ps4ViewDevice::Init(const VideoMode& mode)
{
    _source = ps4::Ps4CaptureSource::Acquire(_portPath);
    if (!_source)
        return false;
    _useIsp = _source->UsesIsp();

    // Ask the shared engine for the per-eye geometry this view needs. A
    // side-by-side view is two eyes wide, so its requested width halves.
    // ONE geometry serves the whole physical camera: only the view that owns the
    // shared controls gets to choose, and the other half of a split pair adopts
    // it (and reports it through ActualMode, so the FrameBus is sized for the
    // frames that actually arrive).
    if (_useIsp)
    {
        const uint32_t wantEyeW = (_view == Ps4ViewKind::SideBySide) ? mode.width / 2 : mode.width;
        if (_ownsControls)
            _source->RequestGeometry(wantEyeW, mode.height, mode.fps);
        const ps4::IspGeometry g = _source->Geometry();
        _width  = (_view == Ps4ViewKind::SideBySide) ? g.eyeW * 2 : g.eyeW;
        _height = g.eyeH;
        _fps    = g.fps;
    }
    else
    {
        _width  = mode.width;
        _height = mode.height;
        _fps    = mode.fps;
    }

    // Allocate for the LARGEST view (SideBySide 2560x800) so the view can be
    // switched live (SetView) without reallocating or restarting the stream.
    const size_t maxOut = framebus::Yuy2Bytes(2 * ps4::kEyeWidth, ps4::kEyeHeight);
    _yuy2Out.reset(new (std::nothrow) uint8_t[maxOut]);
    if (!_yuy2Out)
        return false;
    // Black-fill so a client attaching before the first frame sees clean black.
    yuv::FillBlack(_yuy2Out.get(), 2 * ps4::kEyeWidth, ps4::kEyeHeight, maxOut);

    // Both eyes always (any view can be selected live). The per-eye SBS stitch
    // scratch (_eyeYuyvL/_eyeYuyvR, ~2 MB each) is allocated lazily on first
    // Side-by-side render — a single-eye view, and every split half (which can never
    // present SBS), never touches it, so it isn't reserved here.
    _bayerL.assign(ps4::kEyeBayerBytes, 0);
    _bayerR.assign(ps4::kEyeBayerBytes, 0);
    _toneL.Reset();
    _toneR.Reset();
    _lastSeq = 0;
    return true;
}

// Switch Left/Right/SideBySide live, with NO device interaction: both eyes are in
// every frame, so this only changes which band(s) AcquireFrame composes (and the
// output geometry). The stream keeps running, so there is no iso restart to
// degrade the OV580. The CaptureController re-advertises the geometry separately.
bool Ps4ViewDevice::SetView(int viewKind)
{
    const Ps4ViewKind v = static_cast<Ps4ViewKind>(viewKind);
    _view = v;
    const ps4::IspGeometry g = _source ? _source->Geometry() : ps4::DefaultIspGeometry();
    _width  = (v == Ps4ViewKind::SideBySide) ? g.eyeW * 2 : g.eyeW;
    _height = g.eyeH;
    return true;
}

bool Ps4ViewDevice::Start()
{
    if (!_source || _streaming)
        return false;
    if (!_source->StartStreaming())
        return false;
    _streaming = true;
    return true;
}

void Ps4ViewDevice::Stop()
{
    if (_streaming && _source)
    {
        _source->StopStreaming();
        _streaming = false;
    }
}

bool Ps4ViewDevice::AcquireFrame(AcquiredFrame& out, uint32_t timeoutMs, bool /*wantJpeg*/)
{
    if (!_source)
        return false;

    // Block up to timeoutMs for a frame newer than the one we last rendered.
    const uint64_t seq = _source->WaitNewer(_lastSeq, timeoutMs);
    if (seq == _lastSeq)
        return false;   // nothing new within the timeout

    const uint64_t got =
        (_view == Ps4ViewKind::Left)  ? _source->SnapshotPlanes(_bayerL.data(), nullptr)
      : (_view == Ps4ViewKind::Right) ? _source->SnapshotPlanes(nullptr, _bayerR.data())
                                      :  _source->SnapshotPlanes(_bayerL.data(), _bayerR.data());
    if (got == 0)
        return false;   // no frame assembled yet
    _lastSeq = seq;

    // In ISP mode the snapshotted planes are already calibrated YUY2 from the
    // OV580 ISP, so the view just copies (and optionally flips) them — no
    // demosaic. In RAW mode they are Bayer and need the GRBG demosaic.
    // Per-eye size comes from the engine's committed mode, not a constant: the
    // OV580 delivers 1280x800, 640x400 and 320x192 and every buffer here is
    // sized for the largest, so the smaller modes use a prefix.
    const ps4::IspGeometry g = _source->Geometry();
    const uint32_t eyeW = _useIsp ? g.eyeW : ps4::kEyeWidth;
    const uint32_t eyeH = _useIsp ? g.eyeH : ps4::kEyeHeight;

    if (_view == Ps4ViewKind::Left)
    {
        if (_useIsp)
            Yuy2View(_bayerL.data(), _yuy2Out.get(), eyeW, eyeH, _flipH, _flipV);
        else
            ps4::DemosaicBggrToYuy2(_bayerL.data(), ps4::kEyeWidth, ps4::kEyeHeight,
                                    _yuy2Out.get(), _toneL, _flipH, _flipV);
    }
    else if (_view == Ps4ViewKind::Right)
    {
        if (_useIsp)
            Yuy2View(_bayerR.data(), _yuy2Out.get(), eyeW, eyeH, _flipH, _flipV);
        else
            ps4::DemosaicBggrToYuy2(_bayerR.data(), ps4::kEyeWidth, ps4::kEyeHeight,
                                    _yuy2Out.get(), _toneR, _flipH, _flipV);
    }
    else // SideBySide -> [Left | Right] in one 2560x800 frame
    {
        // Lazy-allocate the per-eye stitch scratch on first SBS render (Init skips
        // it, so single-eye and split views never reserve it).
        if (_eyeYuyvL.empty())
        {
            // Always the LARGEST eye, so a live mode change never reallocates.
            const size_t eyeBytes = framebus::Yuy2Bytes(ps4::kEyeWidth, ps4::kEyeHeight);
            _eyeYuyvL.assign(eyeBytes, 0);
            _eyeYuyvR.assign(eyeBytes, 0);
        }
        if (_useIsp)
        {
            Yuy2View(_bayerL.data(), _eyeYuyvL.data(), eyeW, eyeH, _flipH, _flipV);
            Yuy2View(_bayerR.data(), _eyeYuyvR.data(), eyeW, eyeH, _flipH, _flipV);
        }
        else
        {
            ps4::DemosaicBggrToYuy2(_bayerL.data(), ps4::kEyeWidth, ps4::kEyeHeight,
                                    _eyeYuyvL.data(), _toneL, _flipH, _flipV);
            ps4::DemosaicBggrToYuy2(_bayerR.data(), ps4::kEyeWidth, ps4::kEyeHeight,
                                    _eyeYuyvR.data(), _toneR, _flipH, _flipV);
        }
        const size_t halfRow = (size_t)eyeW * 2;             // bytes per eye row
        const size_t fullRow = (size_t)_width * 2;           // both eyes
        for (uint32_t y = 0; y < eyeH; ++y)
        {
            uint8_t* dst = _yuy2Out.get() + (size_t)y * fullRow;
            memcpy(dst,            _eyeYuyvL.data() + (size_t)y * halfRow, halfRow);
            memcpy(dst + halfRow,  _eyeYuyvR.data() + (size_t)y * halfRow, halfRow);
        }
    }

    out.yuy2      = _yuy2Out.get();
    out.yuy2Bytes = framebus::Yuy2Bytes(_width, _height);
    out.jpeg      = nullptr;
    out.jpegBytes = 0;
    return true;
}

void Ps4ViewDevice::ApplySettings(const Settings& s)
{
    // Flip is applied in software during the copy/demosaic. Latched here; takes
    // effect on the next rendered frame. Both run on the capture thread, so no
    // synchronisation is required.
    _flipH = s.flipH;
    _flipV = s.flipV;

    // ISP camera controls (auto-exposure / gain / exposure / brightness). autoGain
    // also drives auto-exposure (per Settings). Pushed to the shared source, which
    // applies them live or at the next bring-up. (No effect in RAW mode.) Only the
    // home view owns the shared ISP's controls; a split pair's Right half skips this
    // so the two halves don't fight over the single physical control state.
    if (_source && _ownsControls)
    {
        ps4::Ps4CaptureSource::IspControls c;
        c.gain             = s.gain;
        c.brightness       = s.brightness;
        c.contrast         = s.contrast;
        c.saturation       = s.saturation;
        c.sharpness        = s.sharpness;
        c.wbTemp           = s.wbTemp;
        c.powerlineFreq    = s.powerlineFreq;
        c.autoWhiteBalance = s.autoWhiteBalance;
        _source->SetControls(c);
        // Separate from SetControls because it is not a live control: writing it
        // needs the stream stopped, so it no-ops unless the value changed.
        _source->SetMicGain(s.micGain);
    }
}

uint32_t Ps4ViewDevice::AudioChannels() const
{
    return (_source && _ownsControls && _source->MicReady())
               ? ps4::Ps4CaptureSource::kMicChannels : 0;
}

uint32_t Ps4ViewDevice::AudioSampleRate() const
{
    return AudioChannels() ? ps4::Ps4CaptureSource::kMicSampleRate : 0;
}

uint32_t Ps4ViewDevice::AudioMeasuredRateMilliHz() const
{
    return AudioChannels() ? _source->AudioMeasuredRateMilliHz() : 0;
}

uint32_t Ps4ViewDevice::ReadAudio(int16_t* dst, uint32_t frames)
{
    if (!_source || !_ownsControls)
        return 0;
    return _source->ReadAudio(dst, frames);
}

bool Ps4ViewDevice::AudioLevels(float* out, uint32_t count)
{
    if (!_source || !_ownsControls || !out ||
        count < ps4::Ps4CaptureSource::kMicChannels)
        return false;
    return _source->AudioLevels(out);
}

uint32_t Ps4ViewDevice::AudioDropouts() const
{
    return (_source && _ownsControls) ? _source->MicDropouts() : 0;
}

const DeviceProfile& Ps4ViewDevice::Profile() const
{
    return Ps4ViewProfile(_view);
}
