#pragma once
//
// Ps4CaptureSource — ONE shared capture engine per physical PS4 camera.
//
// This is the refcounted singleton (keyed by USB port path) at the centre of the
// "logical views over a shared source" design. It owns the single libusb handle,
// the firmware upload, the startup replay, the isochronous ring, and the frame
// reassembly. It publishes the latest LEFT and RIGHT raw-Bayer eye planes into a
// multi-reader latch; one or more Ps4ViewDevice adapters (Left / Right / SBS)
// crop+demosaic off that latch into their own virtual cameras.
//
// Two devices never share a handle, and N views fan out read-only off the latch,
// so a single PS4 can be ONE switchable camera or TWO simultaneous cameras
// (split mode) with no extra USB cost. The engine streams only while >=1 view is
// awake (StartStreaming/StopStreaming refcount) and fully releases USB when the
// last view sleeps — preserving the project's zero-idle-CPU discipline.
//
// Why per-eye planes and not one combined frame: the OV580 TIME-MULTIPLEXES the
// two eyes across frames (any given frame populates one 1280-px band and fills
// the other with a constant 0x800a "null"). The reassembler accumulates the most
// recent NON-null rows per eye, so both planes stay current even though the
// hardware only paints one at a time. Heavy work (demosaic) lives in the view,
// off this engine's iso event thread, which stays reassembly-only at ~275 MB/s.
//
#include "Ps4MicDsp.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct libusb_device_handle;
struct libusb_transfer;

namespace ps4 {

// LARGEST per-eye geometry (the 1280x800 primary mode). Every buffer in the
// pipeline is sized for this and the smaller modes simply use a prefix of it, so
// a mode change never reallocates and never races a reader.
constexpr uint32_t kEyeWidth     = 1280;
constexpr uint32_t kEyeHeight    = 800;
constexpr uint32_t kEyeBayerBytes = kEyeWidth * kEyeHeight * 2;   // 2,048,000

// One delivered ISP mode. Every frame index shares the same row shape —
// [32 B header][64 B audio][left eyeW*2][right eyeW*2][trailer] — so the whole
// publish path generalises through this struct instead of constants.
//
// All nine entries are HARDWARE-VERIFIED: each was committed and streamed, and
// the delivered bytes per frame matched the requested geometry. The OV580 is
// NOT limited to 60 fps — that reading comes from a probe that sends the
// 1280x800 dwMaxVideoFrameSize with every
// frame index, which the legacy firmware ignored and the final firmware refused.
struct IspGeometry
{
    uint8_t  frameIndex;   // UVC bFrameIndex
    uint32_t interval;     // dwFrameInterval, 100 ns units
    uint32_t rowPx;        // row width in PIXELS (3448 / 1748 / 898)
    uint32_t rows;         // rows per delivered frame (808 / 408 / 200)
    uint32_t eyeW;         // per-eye image width
    uint32_t eyeH;         // per-eye image height (rows past this are trailer)
    uint32_t fps;

    uint32_t RowBytes()   const { return rowPx * 2; }
    uint32_t BandBytes()  const { return eyeW * 2; }
    uint32_t LeftOff()    const { return 96; }                  // 32 hdr + 64 audio
    uint32_t RightOff()   const { return 96 + BandBytes(); }
    uint32_t FrameBytes() const { return RowBytes() * rows; }
    uint32_t EyeBytes()   const { return eyeW * eyeH * 2; }
    bool SameAs(const IspGeometry& o) const
    { return frameIndex == o.frameIndex && interval == o.interval; }
};

extern const IspGeometry kIspGeometries[];
extern const int         kIspGeometryCount;

// Look up a delivered mode by per-eye size + frame rate; null if unsupported.
const IspGeometry* FindIspGeometry(uint32_t eyeW, uint32_t eyeH, uint32_t fps);
// 1280x800@60 — the default and the largest.
const IspGeometry& DefaultIspGeometry();

class Ps4CaptureSource
{
public:
    // Get (or create) the shared source for a physical camera at `portPath`.
    // Reuses an existing instance so split-mode views share one engine.
    static std::shared_ptr<Ps4CaptureSource> Acquire(const std::string& portPath);

    // Close + drop any cached sources whose port path is NOT in `present`. Called
    // by the registry on a device-change so a physically-removed camera's open USB
    // handle is released (and re-uploaded firmware on next plug). Sources for still-
    // present cameras are kept OPEN across sleep/wake — see the lifecycle note below.
    static void RetireAbsent(const std::vector<std::string>& present);

    ~Ps4CaptureSource();

    // Awake refcount over a PERSISTENT device session. The OV580 only streams
    // cleanly while a single USB session stays open: closing the handle (or
    // toggling the streaming alt-setting) and reopening degrades it to ~10 MB/s and
    // would otherwise demand a physical replug. So the engine opens the device ONCE
    // (firmware + claim + UVC commit) and keeps it open for the camera's lifetime;
    // StartStreaming/StopStreaming only submit/cancel the iso ring (like the PS4's
    // own driver). The handle is closed only on physical removal (RetireAbsent) or
    // process exit. Returns false if the device could not be brought up.
    bool StartStreaming();
    void StopStreaming();

    // Select a delivered ISP mode by per-eye geometry. Called from a view's
    // Init() BEFORE Start(). Re-commits the UVC stream if the device is already
    // open (stopping the iso ring around the change, which is exactly what the
    // hardware mode matrix did ten times in a row without a wedge). Returns
    // false only if the geometry is not one the hardware delivers.
    //
    // ONE geometry per physical camera: a split pair shares this engine, so the
    // owning (home/Left) view decides and the other half adopts — which is why
    // Ps4ViewDevice reports ActualMode() rather than what it asked for.
    bool RequestGeometry(uint32_t eyeW, uint32_t eyeH, uint32_t fps);

    // The geometry currently committed (never null-equivalent; defaults to
    // 1280x800@60 before the first request).
    IspGeometry Geometry() const;

    // True when the engine is in YUYV-ISP mode: the OV580's internal ISP outputs
    // calibrated-colour YUY2 (auto-exposed), so the published planes are ready-to-
    // use YUY2 and the view copies them directly (no demosaic). False = legacy RAW
    // Bayer mode (view demosaics). Set once at construction from PS4_RAW; stable for
    // the source's lifetime, so views can read it without locking.
    bool UsesIsp() const { return _useIsp; }

    // Push camera controls (ISP mode). Maps app Settings to UVC controls:
    // autoExposure=true -> CT_AE_MODE auto; false -> shutter-priority + manual
    // CT_EXPOSURE_ABS, with PU_GAIN; brightness -> PU_BRIGHTNESS. Values are app
    // units: gain 0-63, brightness/contrast/wbTemp 0-255. All four map onto live
    // ISP controls (PU_GAIN, PU_BRIGHTNESS, PU_CONTRAST, PU_WB_TEMPERATURE +
    // its auto flag), all hardware-measured.
    //
    // `autoExposure` and `exposure` are IGNORED: the OV580's shutter is
    // permanently automatic on every AE mode it accepts. For a dark scene (PS
    // Move / light-gun tracking) turn GAIN and brightness down instead.
    //
    // Stored and applied at bring-up; applied live when already streaming. Safe to
    // call from the capture-controller thread. No-op in RAW mode.
    // Grouped rather than passed positionally: this list has grown twice, and
    // both times a control was silently dropped on the way through because the
    // argument order was edited in one place and not another.
    struct IspControls
    {
        uint32_t gain          = 20;    // app units 0..63
        uint32_t brightness    = 127;   // the rest are 0..255
        uint32_t contrast      = 127;
        uint32_t saturation    = 128;
        uint32_t sharpness     = 128;
        uint32_t wbTemp        = 128;
        uint32_t powerlineFreq = 1;     // 0 off, 1 = 50 Hz, 2 = 60 Hz
        bool     autoWhiteBalance = true;
    };
    void SetControls(const IspControls& c);

    // Microphone array gain, 0..100 %. NOT a live control: writing it needs the
    // stream stopped, so this stops, writes and brings the stream back, and
    // no-ops when the value has not changed.
    bool SetMicGain(uint32_t pct);
    uint32_t MicGain() const { return _micGainPct; }

    // ---- microphone ------------------------------------------------------
    // The 4-mic array is NOT a USB-audio function: 64 bytes of AK5703 output ride
    // in every video row, between the 32-byte row header and the left eye band --
    // four 16-byte blocks, one per microphone, each holding 8 int16 LE. EVERY
    // delivered row carries them, trailer rows included. The field reads all
    // zeros until Ak5703BringUp() runs.
    //
    // Only eyeH*8 of the rows*8 delivered slots are real; the
    // rest are padding the row header's byte 0 claims to mark and does not. The
    // decoder consumes every slot and lets the resampler take rows*fps down to
    // kMicSampleRate, which lands on the true content rate by construction --
    // 808 rows -> 800 samples a frame. Do not "optimise" that ratio away.
    //
    // Ps4MicDsp.h keeps the filter independent of USB capture for offline testing.
    static const uint32_t kMicChannels   = 4;
    // Audio discarded after each stream start while the ADC settles: it ramps
    // -20, -28, -39, -48 dBFS over ~400 ms before reaching the room's level, and
    // publishing that is a thump plus a meter pinned to full scale. Expressed as
    // a TIME, not a row count -- rows/second is mode-dependent, so the old fixed
    // 24000 rows meant 0.49 s at 1280x800@60 but 2.0 s at 320x192@60.
    static const uint32_t kMicSettleMs   = 500;
    // A dropped VIDEO frame takes a frame of audio with it. Concealing more than
    // a few in a row would be inventing audio, so past this we let the gap
    // through. Drops are detected from the UVC PTS, not the host clock.
    static const uint32_t kMicMaxConceal = 4;   // frames
    // The rate the DSP RESAMPLES TO. The rate actually delivered differs by the
    // camera's true frame rate -- 60.029 fps, so 48,023 Hz at 1280x800@60 and
    // per-mode besides. AudioMeasuredRate() reports what is really coming out.
    static const uint32_t kMicSampleRate = 48000;

    // Copy up to `frames` interleaved 4-channel sample frames out of the ring,
    // oldest first. Returns how many were copied. Non-blocking; a caller that
    // falls more than one second behind loses the oldest audio.
    uint32_t ReadAudio(int16_t* dst, uint32_t frames);

    // Per-channel RMS of the most recent video frame's audio, 0..1. Cheap enough
    // to poll from a UI timer. Returns false if no audio has been published.
    bool AudioLevels(float out[kMicChannels]) const;

    // The rate audio is REALLY leaving at, in milli-Hz, measured against QPC
    // over the session; 0 until enough has been published to be meaningful.
    // kMicSampleRate is what the resampler targets, but the camera runs at
    // 60.029 fps rather than 60.000, so the truth is ~48,023 Hz
    // at 1280x800@60 and differs again per mode -- the two 640x400 modes share a
    // line rate 533 ppm away from 1280x800@60. A recording written at the
    // nominal rate plays slow; a live render into a fixed-rate endpoint drifts
    // until it underruns. Both need this number.
    uint32_t AudioMeasuredRateMilliHz() const
    { return _micRateMilliHz.load(std::memory_order_relaxed); }

    // True once the ADC bring-up has run on this session.
    bool MicReady() const { return _micReady.load(std::memory_order_relaxed); }
    // Gaps concealed since the stream started. Non-zero means video frames are
    // being dropped, which is heard on speech -- see PublishAudio.
    uint32_t MicDropouts() const { return _micDropouts.load(std::memory_order_relaxed); }

    // Block up to timeoutMs for a frame newer than lastSeq. Returns the current
    // seq (== lastSeq if it timed out with nothing new). Multi-reader safe.
    uint64_t WaitNewer(uint64_t lastSeq, uint32_t timeoutMs);

    // Copy the latest Bayer plane(s) into caller buffers (each sized
    // kEyeBayerBytes; pass nullptr to skip an eye). Non-blocking. Returns the
    // seq of the snapshot, or 0 if no frame has been assembled yet.
    uint64_t SnapshotPlanes(uint8_t* leftDst, uint8_t* rightDst);


private:
    explicit Ps4CaptureSource(std::string portPath);
    Ps4CaptureSource(const Ps4CaptureSource&) = delete;
    Ps4CaptureSource& operator=(const Ps4CaptureSource&) = delete;

    bool EnsureOpen();       // open + firmware + claim + alt + UVC commit, ONCE (persists)
    bool StartIso();         // submit the iso ring (per wake); device stays open
    void StopIso();          // cancel the iso ring (per sleep); KEEPS the handle open
    void CloseDevice();      // full teardown: stop iso + release + close (removal/exit)

    bool EnsureRunningDevice();   // firmware upload if in boot state; returns 058A handle ready
    bool BringUpIsp();            // UVC probe/commit (YUYV) + auto-exposure (ISP mode)
    void ApplyControls();         // send the stored ISP controls to _h (holds _stateMutex caller)
    bool Ak5703BringUp();         // power up + configure the mic ADC (once per session,
                                  // while interface 1 is still on alt 0 -- see the call site)
    void PublishAudio(const uint8_t* frame, uint32_t pts, bool ptsValid);
    void ResetMicDsp();                        // rebuild the filter for _geom, clear state
    void SubmitIsoRing();
    void FreeTransfers();

    static void OnIso(libusb_transfer* xfer);
    void HandleIso(libusb_transfer* xfer);

    // Payload depacketiser (per iso packet) -> rolling stream; then frame sync.
    void OnPayload(const uint8_t* d, int len);
    void ExtractFrames();    // cut synced frames out of _stream  (RAW mode)
    bool ResyncLocked();     // (re)find row phase + frame boundary in _stream
    // Reassemble one clean frame's non-null eye bands (at _stream[start]) into
    // the published planes (RAW mode).
    void ReassembleAndPublish(const uint8_t* frame);

    // ISP-mode depacketiser: FID-delimited full YUYV frames (one image between FID
    // toggles), then split the left/right YUYV bands straight into the YUY2 planes.
    void OnPayloadIsp(const uint8_t* d, int len);
    void PublishIspFrame(const uint8_t* frame, uint32_t pts, bool ptsValid);

    const std::string _portPath;
    std::string       _key;        // map key (== _portPath) for self-erase

    // ---- streaming state (guarded by _stateMutex) ----
    mutable std::mutex    _stateMutex;   // mutable: Geometry() is a const accessor
    int                   _awakeUsers = 0;
    bool                  _streaming  = false;   // iso ring submitted
    bool                  _open       = false;   // device opened + set up (handle held)
    bool                  _ctxAcquired = false;
    const bool            _useIsp;       // YUYV ISP (default) vs legacy RAW Bayer
    libusb_device_handle* _h = nullptr;

    // ISP camera controls (guarded by _ctrlMutex). Cached UVC entity ids are
    // resolved once at bring-up. Defaults match the app Settings defaults.
    std::mutex _ctrlMutex;
    IspControls _ctrl;

    // The committed ISP mode. Written only under _stateMutex with the iso ring
    // stopped, so the ISO EVENT THREAD never sees it change mid-frame (that is
    // what makes the unlocked reads in PublishIspFrame / PublishAudio safe).
    IspGeometry _geom{};

    // Per-eye plane size, republished whenever _geom changes.
    //
    // SnapshotPlanes runs on a view's capture thread, which the iso-ring-stopped
    // argument above does not cover. It must NOT take _stateMutex to read _geom:
    // StopIso holds _stateMutex while waiting for the ring to drain, and that
    // drain runs through PublishIspFrame, which takes _latchMutex -- so blocking
    // on _stateMutex while holding _latchMutex deadlocks teardown.
    std::atomic<uint32_t> _eyeBytes{ kEyeBayerBytes };

    // ---- microphone ring -------------------------------------------------
    // One second of 4ch/48 kHz int16 (384 KB). Written only by the iso event
    // thread in PublishAudio, read by any consumer under _micMutex — the buffer
    // is small and the copy is a memcpy, so a plain mutex is cheaper than the
    // seqlock the video planes need.
    static const uint32_t kMicRingFrames = kMicSampleRate;
    mutable std::mutex   _micMutex;
    std::vector<int16_t> _micRing;          // kMicRingFrames * kMicChannels
    uint64_t             _micWritePos = 0;  // total frames ever written
    uint64_t             _micReadPos  = 0;  // total frames ever handed out
    std::atomic<bool>    _micReady{false};
    uint32_t             _micSettleRows = 0;   // iso thread only; set by StartIso
    // Output-rate estimator, iso thread only apart from the published atomic.
    // A SLIDING window, not a running average since stream start: a cumulative
    // one never recovers from a capture interruption, and read -374 ppm against
    // a true +487 for minutes afterwards. Restarted whenever the conceal count
    // moves (audio was lost, so the count is not a rate any more) or the window
    // reaches 20 s.
    long long            _micRateT0 = 0;       // QPC at the window start
    uint64_t             _micRateFrames = 0;   // samples published within it
    uint32_t             _micRateDrops = 0;    // conceal count the window started at
    std::atomic<uint32_t> _micRateMilliHz{0};
    uint32_t             _micGainPct = 60;     // guarded by _stateMutex
    long long            _micLastQpc = 0;      // iso thread only
    long long            _micQpcFreq = 0;
    // Decimation DSP, iso thread only; shared with offline signal tests.
    ps4mic::MicDsp       _micDsp;
    std::vector<int16_t> _micFrameBuf;   // one frame of output, iso thread only
    // Drop detection from the UVC presentation timestamp rather than the host
    // clock. The 12-byte iso header carries PTS and SCR (bmHeaderInfo bits 2
    // and 3, both set on this firmware). The host-clock rule is honest at 60 and
    // 120 fps but fires on jitter at 240, where the frame period is 4.2 ms --
    // it concealed twice against one real gap, fabricating 200 samples.
    uint32_t             _ispFramePts = 0;         // iso thread only
    bool                 _ispFramePtsValid = false;
    uint32_t             _micLastPts = 0;
    bool                 _micLastPtsValid = false;
    uint32_t             _micPtsPerFrame = 0;      // running minimum = true period
    std::atomic<uint32_t> _micDropouts{0};     // concealed gaps this session
    uint8_t  MicGainReg() const;
    void     WriteMicGain();                   // caller holds _stateMutex, alt 0
    std::atomic<uint32_t> _micLevel[kMicChannels] = {};   // RMS * 10000, per channel
    uint8_t    _ctEntity = 0;     // Camera Terminal (AE mode / exposure)
    uint8_t    _puEntity = 0;     // Processing Unit  (gain / brightness)

    std::vector<libusb_transfer*>     _xfers;
    std::vector<std::vector<uint8_t>> _xferBufs;
    std::atomic<bool>                 _cancelling{ false };
    std::atomic<int>                  _xfersInFlight{ 0 };

    // ---- depacketiser state (touched ONLY on the iso event thread) ----
    // Rolling header-stripped video stream + frame sync. Every clean row begins
    // with [validMask][0x88][0x88]; a frame is exactly kFrameRows rows; the
    // frame-boundary row has validMask==0x00. We lock onto a frame start (row
    // phase via the 0x88 0x88 signature + the boundary marker) then cut fixed
    // frames, re-syncing if the expected signature ever drifts.
    std::vector<uint8_t> _stream;          // rolling stripped payload
    bool                 _synced = false;
    size_t               _frameStart = 0;  // index in _stream of the current frame's row 0
    int                  _ispFid = -1;     // ISP mode: last UVC frame-id bit (frame delimiter)

    // ---- published per-eye latch (guarded by _latchMutex) ----
    std::mutex              _latchMutex;
    std::condition_variable _latchCv;
    std::vector<uint8_t>    _pubLeft;    // kEyeBayerBytes
    std::vector<uint8_t>    _pubRight;   // kEyeBayerBytes
    uint64_t                _seq = 0;    // ++ per published frame; 0 = none yet
};

} // namespace ps4
