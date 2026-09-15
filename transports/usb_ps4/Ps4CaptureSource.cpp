#include "Ps4CaptureSource.h"
#include "Ps4Ak5703Init.h"

#include <libusb.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <thread>

#include "Ps4Usb.h"
#include "Ps4Firmware.h"
#include "../../common/ControlScale.h"

namespace ps4 {

// Hardware-verified delivered modes (see IspGeometry in the header). Ordered
// largest-first so the default is entry 0.
const IspGeometry kIspGeometries[] = {
    //             idx  interval  rowPx rows  eyeW eyeH  fps
    IspGeometry{ 1, 166666,  3448, 808, 1280, 800,  60 },
    IspGeometry{ 1, 333333,  3448, 808, 1280, 800,  30 },
    IspGeometry{ 1, 666666,  3448, 808, 1280, 800,  15 },
    IspGeometry{ 2,  83333,  1748, 408,  640, 400, 120 },
    IspGeometry{ 2, 166666,  1748, 408,  640, 400,  60 },
    IspGeometry{ 2, 333333,  1748, 408,  640, 400,  30 },
    IspGeometry{ 3,  41666,   898, 200,  320, 192, 240 },
    IspGeometry{ 3,  83333,   898, 200,  320, 192, 120 },
    IspGeometry{ 3, 166666,   898, 200,  320, 192,  60 },
};
const int kIspGeometryCount = (int)(sizeof(kIspGeometries) / sizeof(kIspGeometries[0]));

const IspGeometry* FindIspGeometry(uint32_t eyeW, uint32_t eyeH, uint32_t fps)
{
    for (int i = 0; i < kIspGeometryCount; ++i)
        if (kIspGeometries[i].eyeW == eyeW && kIspGeometries[i].eyeH == eyeH &&
            kIspGeometries[i].fps == fps)
            return &kIspGeometries[i];
    return nullptr;
}

const IspGeometry& DefaultIspGeometry() { return kIspGeometries[0]; }

namespace {

// Clean-row layout (confirmed on hardware). Every row begins with a small
// header whose first byte is a per-row "valid mask" followed by the constant
// signature 0x88 0x88; a frame's boundary row has validMask==0x00.
//   [32 hdr][64 data][LEFT 1280x2 @96][RIGHT 1280x2 @2656][840 trailer] = 6056 B.
constexpr uint32_t kRowHeader   = 32;
constexpr uint32_t kRowData     = 64;
constexpr uint32_t kBandBytes   = kEyeWidth * 2;                 // 2560
constexpr uint32_t kLeftOffset  = kRowHeader + kRowData;         // 96
constexpr uint32_t kRightOffset = kLeftOffset + kBandBytes;      // 2656
constexpr uint32_t kCleanRow    = kRightOffset + kBandBytes + 840; // 6056
// One image is exactly kFrameRows clean rows (frame starts repeat every 811 rows
// on hardware: a few warm-up rows whose validMask ramps 02->ff, then the image,
// then the validMask==0x00 boundary row). The eye image occupies the first
// kEyeHeight rows of the frame.
constexpr uint32_t kFrameRows   = 811;
constexpr uint32_t kFrameBytes  = kCleanRow * kFrameRows;       // 4,911,416 (one image)
constexpr uint32_t kMaxStream   = kFrameBytes * 3;              // rolling-buffer cap

// iso ring (EP 0x81). 16 packets x 8 transfers gives ~22 ms of buffering at the
// camera's ~275 MB/s, enough to ride scheduling jitter in a background app.
constexpr uint8_t kIsoEp   = 0x81;
constexpr int     kIsoPkts = 16;
constexpr int     kIsoRing = 8;

// ---- YUYV ISP-mode layout (confirmed on hardware) -------------------------
// The OV580's internal ISP outputs UVC uncompressed YUY2. Every row of every
// mode has the same shape: [32 hdr][64 audio][left eyeW*2][right eyeW*2][trailer],
// and both eyes are painted EVERY frame (no time-multiplex), so split/SBS get
// true simultaneous stereo. The eye image occupies the first eyeH rows and each
// band is exactly eyeW YUY2 pixels, copied straight to the FrameBus with zero
// demosaic.
//
// The per-mode numbers live in ps4::kIspGeometries (above) rather than as
// constants here: the OV580 delivers 1280x800, 640x400 and 320x192 at several
// rates each, and every one of them is hardware-verified.

// UVC requests / video-streaming controls that put the bridge in YUYV mode.
constexpr uint8_t  kUvcSetCur     = 0x01;
constexpr uint16_t kVsProbe       = 0x0100;   // VS_PROBE_CONTROL  << 8
constexpr uint16_t kVsCommit      = 0x0200;   // VS_COMMIT_CONTROL << 8
constexpr uint8_t  kCtAeModeCs    = 0x02;     // CT_AE_MODE_CONTROL selector
constexpr uint8_t  kPuBrightCs    = 0x02;     // PU_BRIGHTNESS_CONTROL
constexpr uint8_t  kPuGainCs      = 0x04;     // PU_GAIN_CONTROL
constexpr uint8_t  kPuContrastCs  = 0x03;     // PU_CONTRAST_CONTROL
constexpr uint8_t  kPuWbTempCs    = 0x0a;     // PU_WHITE_BALANCE_TEMPERATURE_CONTROL
constexpr uint8_t  kPuSaturationCs = 0x07;   // live: 0 = true monochrome, 8 = ~2x chroma
constexpr uint8_t  kPuSharpnessCs  = 0x08;   // live: 0..8 moves mean |dY/dx| 3.05 -> 5.14
constexpr uint8_t  kPuPowerlineCs  = 0x05;   // 0 off, 1 = 50 Hz, 2 = 60 Hz
constexpr uint8_t  kPuWbAutoCs    = 0x0b;     // PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL
constexpr uint8_t  kAeModeAuto    = 0x02;     // UVC AE bitmap: auto-exposure

void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

inline void Put16(uint8_t* b, uint16_t v) { b[0] = v & 0xff; b[1] = v >> 8; }
inline void Put32(uint8_t* b, uint32_t v) { b[0]=v&0xff; b[1]=(v>>8)&0xff; b[2]=(v>>16)&0xff; b[3]=(v>>24)&0xff; }

// Find the Camera Terminal entity id (ITT_CAMERA) from the VideoControl
// interface's class-specific descriptors; 0 if not found.
uint8_t FindCameraTerminal(libusb_device* dev)
{
    uint8_t id = 0;
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_config_descriptor(dev, 0, &cfg) != 0)
        return 0;
    for (int i = 0; i < cfg->bNumInterfaces && !id; ++i)
    {
        const libusb_interface& itf = cfg->interface[i];
        for (int a = 0; a < itf.num_altsetting && !id; ++a)
        {
            const libusb_interface_descriptor& d = itf.altsetting[a];
            if (d.bInterfaceClass != 0x0E || d.bInterfaceSubClass != 0x01) continue; // VideoControl
            const uint8_t* p = d.extra; int rem = d.extra_length;
            while (rem >= 3)
            {
                const int len = p[0], type = p[1], sub = p[2];
                if (len < 3 || len > rem) break;
                if (type == 0x24 && sub == 0x02 /*VC_INPUT_TERMINAL*/ &&
                    (uint16_t)(p[4] | (p[5] << 8)) == 0x0201 /*ITT_CAMERA*/)
                    id = p[3];
                p += len; rem -= len;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return id;
}

// Find the Processing Unit entity id from the VideoControl descriptors; 0 if none.
uint8_t FindProcessingUnit(libusb_device* dev)
{
    uint8_t id = 0;
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_config_descriptor(dev, 0, &cfg) != 0)
        return 0;
    for (int i = 0; i < cfg->bNumInterfaces && !id; ++i)
    {
        const libusb_interface& itf = cfg->interface[i];
        for (int a = 0; a < itf.num_altsetting && !id; ++a)
        {
            const libusb_interface_descriptor& d = itf.altsetting[a];
            if (d.bInterfaceClass != 0x0E || d.bInterfaceSubClass != 0x01) continue;
            const uint8_t* p = d.extra; int rem = d.extra_length;
            while (rem >= 3)
            {
                const int len = p[0], type = p[1], sub = p[2];
                if (len < 3 || len > rem) break;
                if (type == 0x24 && sub == 0x05 /*VC_PROCESSING_UNIT*/) id = p[3];
                p += len; rem -= len;
            }
        }
    }
    libusb_free_config_descriptor(cfg);
    return id;
}

// One stalled control request WEDGES the OV580's control pipe: every request
// after it fails too, so a single unsupported selector silently kills every
// control written after it. It is therefore ORDER-DEPENDENT: the same probe run
// in a different sequence reports a different set of "unsupported" controls,
// and a wall measured through a wedged pipe is not a wall. After any failure,
// clear the halt
// and resynchronise with a request the device always answers (VS_PROBE
// GET_CUR), so the NEXT control write starts from a healthy pipe.
void UvcPipeRecover(libusb_device_handle* h)
{
    libusb_clear_halt(h, 0x00);
    uint8_t probe[26] = {};
    libusb_control_transfer(h, 0xA1, 0x81, kVsProbe, 1, probe, 26, 500);
}

// ---- vendor register channel (OV580 0xa4) ---------------------------------
// PAYLOAD layout: [0] = mode byte (per subaddress), [1..7] = 0, then one 4-byte
// record per register {reg_lo, reg_hi, value, subaddr} starting at offset 8;
// transfer length = 8 + 4*n.
//
// The reference driver's buffer offsets look different (its [8] and [16]) only
// because it builds for libusb_fill_control_setup, whose first EIGHT bytes are
// the control SETUP packet. Copying those offsets into a libusb_control_transfer
// payload — which already excludes the setup packet — puts the mode byte inside
// the first record and the record itself in the third slot. The device ACKs such
// a transfer and does nothing, which is exactly how the microphone, the LED and
// the sensor registers all looked like hardware walls for months.
int RegWriteMulti(libusb_device_handle* h, const ps4::Ak5703Reg* regs, int n,
                  uint8_t subaddr, uint8_t mode)
{
    if (n < 1) return 0;
    std::vector<uint8_t> buf((size_t)8 + 4 * n, 0);
    buf[0] = mode;
    for (int i = 0; i < n; ++i)
    {
        buf[8 + 4 * i + 0] = (uint8_t)(regs[i].reg & 0xff);
        buf[8 + 4 * i + 1] = (uint8_t)(regs[i].reg >> 8);
        buf[8 + 4 * i + 2] = regs[i].val;
        buf[8 + 4 * i + 3] = subaddr;
    }
    int r = libusb_control_transfer(h, 0x40, 0xa4, 0, 0, buf.data(),
                                    (uint16_t)buf.size(), 1000);
    if (r < 0)
    {
        // Same stall discipline as UvcSetCur. This channel needed it MORE, not
        // less: the AK5703 bring-up writes 72 registers back to back, so without
        // recovery a single stall silently discards the other 71 and the mic
        // comes up dead with every transfer having "succeeded" up to that point.
        UvcPipeRecover(h);
        r = libusb_control_transfer(h, 0x40, 0xa4, 0, 0, buf.data(),
                                    (uint16_t)buf.size(), 1000);
        if (r < 0)
            UvcPipeRecover(h);
    }
    return r;
}

// Read one register back (0xa5 sets up the address, 0xa6 fetches it; the value
// lands at payload [10]). Same 8-byte-offset rule as the write path.
int RegRead(libusb_device_handle* h, uint16_t reg, uint8_t subaddr, uint8_t mode, uint8_t& val)
{
    uint8_t buf[12] = {};
    buf[0]  = mode;
    buf[8]  = (uint8_t)(reg & 0xff);
    buf[9]  = (uint8_t)(reg >> 8);
    buf[11] = subaddr;
    int r = libusb_control_transfer(h, 0x40, 0xa5, 0, 0, buf, sizeof(buf), 1000);
    if (r < 0)
    {
        UvcPipeRecover(h);
        return r;
    }
    uint8_t in[12] = {};
    r = libusb_control_transfer(h, 0xC0, 0xa6, 0, 0, in, sizeof(in), 1000);
    if (r < 0)
        UvcPipeRecover(h);
    else
        val = in[10];
    return r;
}

// SET_CUR a control (selector cs of entity) over the VideoControl interface.
int UvcSetCur(libusb_device_handle* h, uint8_t cs, uint8_t entity, const uint8_t* data, int len)
{
    int r = libusb_control_transfer(h, 0x21, kUvcSetCur, (uint16_t)cs << 8, (uint16_t)entity << 8,
                                    const_cast<uint8_t*>(data), (uint16_t)len, 1000);
    if (r < 0)
    {
        UvcPipeRecover(h);
        r = libusb_control_transfer(h, 0x21, kUvcSetCur, (uint16_t)cs << 8, (uint16_t)entity << 8,
                                    const_cast<uint8_t*>(data), (uint16_t)len, 1000);
        if (r < 0)
            UvcPipeRecover(h);
    }
    return r;
}
// GET_* a control (req: CUR=0x81 MIN=0x82 MAX=0x83 DEF=0x87); returns the LE
// value, or fallback if the request fails even after a pipe recovery.
long UvcGetReq(libusb_device_handle* h, uint8_t req, uint8_t cs, uint8_t entity, int len, long fallback)
{
    uint8_t b[4] = {};
    int r = libusb_control_transfer(h, 0xA1, req, (uint16_t)cs << 8, (uint16_t)entity << 8, b, (uint16_t)len, 1000);
    if (r < 0)
    {
        UvcPipeRecover(h);
        r = libusb_control_transfer(h, 0xA1, req, (uint16_t)cs << 8, (uint16_t)entity << 8, b, (uint16_t)len, 1000);
        if (r < 0)
            UvcPipeRecover(h);
    }
    if (r < 0)
        return fallback;
    long v = 0;
    for (int i = 0; i < len; ++i) v |= (long)b[i] << (8 * i);
    return v;
}
// GET_MAX: a reported maximum of 0 is meaningless for scaling, so it falls back.
long UvcGetMax(libusb_device_handle* h, uint8_t cs, uint8_t entity, int len, long fallback)
{
    const long v = UvcGetReq(h, 0x83, cs, entity, len, fallback);
    return v ? v : fallback;
}
// GET_MIN: 0 IS a legitimate minimum here (brightness and contrast both start at
// 0), so this one must not treat it as a failure.
long UvcGetMin(libusb_device_handle* h, uint8_t cs, uint8_t entity, int len, long fallback)
{ return UvcGetReq(h, 0x82, cs, entity, len, fallback); }
// Shared with the Settings UI so a slider's label cannot disagree with what the
// device is told. A negative maximum means GET_MAX failed; treat it as zero.
uint32_t Scale(uint32_t v, uint32_t inMax, long devMax)
{ return ScaleToDevice(v, inMax, devMax < 0 ? 0u : (uint32_t)devMax); }

// A "null" band is the 0x800a constant fill the OV580 writes for the eye it is
// not painting this frame. Sample a few positions instead of scanning 2560 B.
inline bool BandIsNull(const uint8_t* p)
{
    auto s = [&](uint32_t i) -> uint16_t { return (uint16_t)(p[i * 2] | (p[i * 2 + 1] << 8)); };
    return s(0) == 0x800a && s(640) == 0x800a && s(1279) == 0x800a;
}

// Replay the captured startup control-transfer script (8-byte setup + data per
// record) to bring the sensors up to streaming.
void ReplayStartup(libusb_device_handle* h, const std::vector<uint8_t>& cmds)
{
    size_t p = 0;
    while (p + 8 <= cmds.size())
    {
        const uint8_t  bmReq = cmds[p + 0];
        const uint8_t  bReq  = cmds[p + 1];
        const uint16_t wVal  = (uint16_t)(cmds[p + 2] | (cmds[p + 3] << 8));
        const uint16_t wIdx  = (uint16_t)(cmds[p + 4] | (cmds[p + 5] << 8));
        const uint16_t wLen  = (uint16_t)(cmds[p + 6] | (cmds[p + 7] << 8));
        p += 8;
        if (p + wLen > cmds.size()) break;

        std::vector<uint8_t> buf(wLen ? wLen : 1);
        if (wLen && !(bmReq & LIBUSB_ENDPOINT_IN))
            memcpy(buf.data(), cmds.data() + p, wLen);
        SleepMs(2);
        libusb_control_transfer(h, bmReq, bReq, wVal, wIdx, buf.data(), wLen, 1000);
        p += wLen;
    }
}

// Registry of shared sources keyed by port path (one engine per camera). STRONG
// refs: a source persists (USB handle kept open, firmware loaded) across view
// sleep/wake so the OV580 stays in its one clean session. Entries are dropped only
// by RetireAbsent (physical removal) or process exit.
//
// Both the map and its mutex are function-local statics, and Sources() touches
// UsbContext::Instance() BEFORE constructing the map. That forces the libusb
// context singleton to be constructed first and therefore destroyed LAST — so at
// process exit the map (and every source's ~CloseDevice, which calls back into
// UsbContext::Release) tears down while the context is still alive. Without this
// ordering the namespace-static map outlived the context singleton and the dtor
// dereferenced freed memory (0xC0000005 on shutdown).
std::mutex& SourcesMutex()
{
    static std::mutex m;
    return m;
}
std::map<std::string, std::shared_ptr<Ps4CaptureSource>>& Sources()
{
    UsbContext::Instance();   // construct the ctx first => destroyed after this map
    static std::map<std::string, std::shared_ptr<Ps4CaptureSource>> s;
    return s;
}

} // namespace

std::shared_ptr<Ps4CaptureSource> Ps4CaptureSource::Acquire(const std::string& portPath)
{
    std::lock_guard<std::mutex> lk(SourcesMutex());
    std::shared_ptr<Ps4CaptureSource>& sp = Sources()[portPath];
    if (!sp)
        sp.reset(new Ps4CaptureSource(portPath));
    return sp;
}

void Ps4CaptureSource::RetireAbsent(const std::vector<std::string>& present)
{
    // Collect the to-close sources, then destroy them OUTSIDE the lock (their
    // dtor closes USB, which can take a moment, and must not re-enter the lock).
    std::vector<std::shared_ptr<Ps4CaptureSource>> retiring;
    {
        std::lock_guard<std::mutex> lk(SourcesMutex());
        auto& sources = Sources();
        for (auto it = sources.begin(); it != sources.end(); )
        {
            const bool stillHere =
                std::find(present.begin(), present.end(), it->first) != present.end();
            if (stillHere) { ++it; continue; }
            retiring.push_back(std::move(it->second));
            it = sources.erase(it);
        }
    }
    // retiring goes out of scope here: if no view still holds a source, it is
    // destroyed and its handle closed; otherwise it dies when the last view does.
}

Ps4CaptureSource::Ps4CaptureSource(std::string portPath)
    : _portPath(std::move(portPath)), _key(_portPath),
      // YUYV ISP is the default (calibrated colour, auto-exposed, both eyes live,
      // zero-CPU). Set PS4_RAW=1 to fall back to the legacy Bayer demosaic path.
      _useIsp(getenv("PS4_RAW") == nullptr)
{
    // A YUY2 1280x800 plane is the same byte size as a 16-bit Bayer plane
    // (1280*800*2 = kEyeBayerBytes), so the latch buffers serve both modes.
    _pubLeft.assign(kEyeBayerBytes, 0);
    _pubRight.assign(kEyeBayerBytes, 0);
    _geom = DefaultIspGeometry();
    _eyeBytes.store(_geom.EyeBytes(), std::memory_order_release);
}

Ps4CaptureSource::~Ps4CaptureSource()
{
    // The map (RetireAbsent) owns removal from g_sources; the dtor only releases
    // USB. Full teardown: cancel iso, release interfaces, close the handle.
    std::lock_guard<std::mutex> lk(_stateMutex);
    CloseDevice();
}

bool Ps4CaptureSource::StartStreaming()
{
    std::lock_guard<std::mutex> lk(_stateMutex);
    if (_awakeUsers++ == 0)
    {
        // Open the device once (firmware + claim + UVC commit); on later wakes the
        // handle is already open, so we only resubmit the iso ring — no close /
        // reopen / alt-toggle, which is what keeps the OV580's session clean.
        if (!_open && !EnsureOpen())
        {
            _awakeUsers = 0;
            return false;
        }
        if (!StartIso())
        {
            _awakeUsers = 0;
            return false;
        }
    }
    return _streaming;
}

void Ps4CaptureSource::StopStreaming()
{
    std::lock_guard<std::mutex> lk(_stateMutex);
    if (_awakeUsers > 0 && --_awakeUsers == 0)
        StopIso();   // keep the handle OPEN for a clean restart (no replug)
}

IspGeometry Ps4CaptureSource::Geometry() const
{
    std::lock_guard<std::mutex> lk(_stateMutex);
    return _geom;
}

// Select a delivered mode. Safe to call while the handle is open: the iso ring
// is stopped around the re-commit, which is exactly the sequence the hardware
// mode matrix ran ten times in a row (alt 0 -> probe/commit -> alt 1) with no
// wedge and no replug. Every buffer is already sized for the LARGEST mode, so
// nothing reallocates and no reader can see a half-resized plane.
bool Ps4CaptureSource::RequestGeometry(uint32_t eyeW, uint32_t eyeH, uint32_t fps)
{
    const IspGeometry* g = FindIspGeometry(eyeW, eyeH, fps);
    if (!g)
        return false;

    std::lock_guard<std::mutex> lk(_stateMutex);
    if (_geom.SameAs(*g) && _geom.eyeW == g->eyeW && _geom.eyeH == g->eyeH)
        return true;                     // already there

    const bool wasStreaming = _streaming;
    if (_open && _useIsp)
    {
        if (wasStreaming)
            StopIso();
        // Drop any partially accumulated frame: it belongs to the OLD geometry
        // and would otherwise be published with the new row stride.
        _stream.clear();
        _ispFid = -1;
        _geom = *g;
        _eyeBytes.store(_geom.EyeBytes(), std::memory_order_release);
        libusb_set_interface_alt_setting(_h, 1, 0);   // zero-bandwidth
        libusb_set_interface_alt_setting(_h, 1, 1);
        if (!BringUpIsp())
        {
            OutputDebugStringW(L"[PSCam4Win] PS4: mode re-commit failed\n");
            return false;
        }
        if (wasStreaming && !StartIso())
            return false;
    }
    else
    {
        _geom = *g;                      // applied by the next EnsureOpen()
        _eyeBytes.store(_geom.EyeBytes(), std::memory_order_release);
    }
    return true;
}

// Ensure a streaming handle exists (058A legacy build / 058B final build),
// uploading firmware to the boot (0580) device first if necessary. Returns true
// with _h set to an opened streaming handle. Both identities expose the same
// VC/VS interfaces and iso EP, so everything downstream is build-agnostic.
// Called once per device session (EnsureOpen); the handle is then kept open across
// sleep/wake, so the firmware upload happens only on a fresh plug, not every wake.
bool Ps4CaptureSource::EnsureRunningDevice()
{
    // Already streaming-capable?
    _h = OpenRunning(_portPath);
    if (_h)
        return true;

    // Still in the bootloader: locate + verify firmware, upload, wait for the
    // streaming identity to re-enumerate.
    FirmwareBlobs blobs = EnsureFirmware();
    if (blobs.firmware.empty())
    {
        OutputDebugStringW(L"[PSCam4Win] PS4: firmware.bin not found/verified -- cannot start\n");
        return false;
    }
    libusb_device_handle* boot = OpenByPortPath(_portPath, kPidBoot);
    if (!boot)
    {
        OutputDebugStringW(L"[PSCam4Win] PS4: boot device (0580) not openable (WinUSB bound?)\n");
        return false;
    }
    UploadFirmware(boot, blobs.firmware);
    libusb_close(boot);

    for (int i = 0; i < 60; ++i)   // up to ~6 s for re-enumeration
    {
        SleepMs(100);
        _h = OpenRunning(_portPath);
        if (_h)
            return true;
    }
    OutputDebugStringW(L"[PSCam4Win] PS4: streaming device (058A/058B) did not "
                       L"re-enumerate after firmware\n");
    return false;
}

// Put the OV580 into YUYV ISP output via standard UVC probe/commit, then turn on
// auto-exposure (the default leaves the image near-black). Confirmed on hardware:
// commit FORMAT_INDEX=1 (YUY2) FRAME_INDEX=1 (3448x808@60), then CT_AE_MODE=auto
// lifts mean luma ~15 -> ~120. Iface 1 alt 1 is already selected by the caller.
bool Ps4CaptureSource::BringUpIsp()
{
    // dwMaxVideoFrameSize MUST match the requested frame index. Sending the
    // 1280x800 size with a smaller index is what makes the 120/240 fps modes
    // fail: the final firmware refuses the commit outright.
    uint8_t buf[26] = {};
    Put16(buf + 0, 0x0001);                  // bmHint = keep dwFrameInterval
    buf[2] = 1;                              // bFormatIndex = 1 (YUY2)
    buf[3] = _geom.frameIndex;               // bFrameIndex
    Put32(buf + 4, _geom.interval);          // dwFrameInterval (100 ns units)
    Put32(buf + 18, _geom.FrameBytes());     // dwMaxVideoFrameSize (per mode!)
    Put32(buf + 22, 49152);                  // dwMaxPayloadTransferSize

    int r = libusb_control_transfer(_h, 0x21, kUvcSetCur, kVsProbe, 1, buf, 26, 1000);
    if (r < 0) return false;
    uint8_t got[26] = {};
    if (libusb_control_transfer(_h, 0xA1, 0x81, kVsProbe, 1, got, 26, 1000) < 0)
        memcpy(got, buf, sizeof(got));
    r = libusb_control_transfer(_h, 0x21, kUvcSetCur, kVsCommit, 1, got, 26, 1000);
    if (r < 0) return false;

    // Resolve the UVC entities once, then push the app's camera controls (default
    // = auto-exposure, which lifts the otherwise near-black ISP output).
    libusb_device* dev = libusb_get_device(_h);
    _ctEntity = FindCameraTerminal(dev);
    _puEntity = FindProcessingUnit(dev);
    ApplyControls();
    return true;
}

// Send the cached camera controls to the device (ISP mode). Single control
// transfers on the VideoControl interface; failures (unsupported selectors) are
// ignored. Called at bring-up (single-threaded) and live from SetControls.
//
// Hardware reality on the final firmware: both AE modes
// the OV580 accepts are AUTO — the shutter-priority(4) path below is accepted
// on the wire, but the ISP's AE loop compensates exposure/gain writes away
// (16x exposure swing + max gain moves mean luma ~1%). The PS4 profile therefore
// advertises neither CTRL_GAIN nor CTRL_EXPOSURE, and the dialog cannot set
// autoExposure=false; the manual path below survives only so a stale registry
// value cannot reach the hardware.
void Ps4CaptureSource::ApplyControls()
{
    if (!_h || !_useIsp)
        return;
    std::lock_guard<std::mutex> lk(_ctrlMutex);

    // AE is pinned to AUTO. The OV580 accepts only modes 2 (auto) and 4
    // (shutter priority), and BOTH are auto — measured on a healthy control
    // pipe, under mode 4 a deliberately LOW exposure with gain pinned to
    // minimum makes the image BRIGHTER (luma 126 -> 156), because mode 4 just
    // targets a higher AE setpoint. Selecting it would therefore reintroduce the
    // very blow-out this camera was reported for, so shutter priority is not
    // switchable: the PS4 profiles advertise neither CTRL_GAIN nor
    // CTRL_EXPOSURE, and a stale AutoGain=0 in the registry must not be able to
    // quietly change the picture. Real exposure control is PU_GAIN below, which
    // is always live.
    if (_ctEntity)
    {
        const uint8_t ae = kAeModeAuto;
        UvcSetCur(_h, kCtAeModeCs, _ctEntity, &ae, 1);
    }

    // The always-live ISP controls. All four measurably move the image on the
    // shipped firmware and none of them needs an "auto off"
    // mode first — the ISP's AE stays on and these ride on top of it.
    if (_puEntity)
    {
        auto put16 = [&](uint8_t cs, long v) {
            uint8_t b[2] = { (uint8_t)(v & 0xff), (uint8_t)((v >> 8) & 0xff) };
            UvcSetCur(_h, cs, _puEntity, b, 2);
        };

        // Brightness: a linear luma offset. Strong, but it CLIPS highlights
        // above device value 3, which is exactly the reported blow-out. Note
        // that Settings' 127 default truncates to exactly 3 here -- do NOT
        // "fix" that to round to the device's own default of 4.
        put16(kPuBrightCs, Scale(_ctrl.brightness, 255, UvcGetMax(_h, kPuBrightCs, _puEntity, 2, 8)));

        // Contrast: a real tone curve (device 0..8). Clips at the top end too.
        put16(kPuContrastCs, Scale(_ctrl.contrast, 255, UvcGetMax(_h, kPuContrastCs, _puEntity, 2, 8)));

        // Gain: the honest exposure trim. Settings::gain is 0..63 (PS3 Eye
        // units) and the ISP takes 0..8; unlike brightness it does not clip at
        // either end, so it is the right knob for matching the PS3 Eye's level.
        put16(kPuGainCs, Scale(_ctrl.gain, 63, UvcGetMax(_h, kPuGainCs, _puEntity, 2, 8)));

        // Saturation and sharpness are both fully live, and both invisible to
        // luma-only photometry — which is why they must be measured in chroma:
        // saturation 0 gives U=V=128.00 exactly (true monochrome) and 8 doubles
        // the chroma magnitude; sharpness 0->8 moves mean |dY/dx| 3.05 -> 5.14.
        put16(kPuSaturationCs, Scale(_ctrl.saturation, 255, UvcGetMax(_h, kPuSaturationCs, _puEntity, 2, 8)));
        put16(kPuSharpnessCs, Scale(_ctrl.sharpness, 255, UvcGetMax(_h, kPuSharpnessCs, _puEntity, 2, 8)));

        // Mains anti-flicker. The ISP powers up on 50 Hz and nothing ever wrote
        // it, so 60 Hz regions banded with no way out.
        {
            const uint8_t pl = (uint8_t)(_ctrl.powerlineFreq > 2 ? 1 : _ctrl.powerlineFreq);
            UvcSetCur(_h, kPuPowerlineCs, _puEntity, &pl, 1);
        }

        // White balance: auto flag first, then the manual temperature only when
        // auto is off (the device rejects the write otherwise).
        const uint8_t wbAuto = _ctrl.autoWhiteBalance ? 1 : 0;
        UvcSetCur(_h, kPuWbAutoCs, _puEntity, &wbAuto, 1);
        if (!_ctrl.autoWhiteBalance)
        {
            const long mn = UvcGetMin(_h, kPuWbTempCs, _puEntity, 2, 2800);
            const long mx = UvcGetMax(_h, kPuWbTempCs, _puEntity, 2, 6500);
            put16(kPuWbTempCs, mn + (long)Scale(_ctrl.wbTemp, 255, mx - mn));
        }
    }
}

void Ps4CaptureSource::SetControls(const IspControls& c)
{
    {
        std::lock_guard<std::mutex> lk(_ctrlMutex);
        // Note what is NOT here: auto-exposure and exposure time. The OV580's
        // shutter is permanently automatic on every AE mode it accepts, so the
        // struct does not carry them at all rather than carrying fields that
        // quietly do nothing.
        _ctrl = c;
    }
    // Apply live if we are already streaming (entities resolved at bring-up).
    // _streaming/_h are guarded by _stateMutex — CloseDevice nulls _h and closes the
    // handle under it — so take it before touching _h. Without it, a teardown on
    // another thread (a split-mode sibling's wake/EnsureOpen failure, or RetireAbsent)
    // could close the handle between this check and the control transfer (handle
    // use-after-free). The _ctrlMutex block above is released first, so the nesting is
    // _stateMutex -> _ctrlMutex (inside ApplyControls), matching bring-up/StartIso.
    std::lock_guard<std::mutex> lk(_stateMutex);
    if (_useIsp && _streaming && _h)
        ApplyControls();
}

// Open the device ONCE and set it up to the point just before iso submission:
// firmware (if in boot state) -> claim -> alt 1 -> UVC commit (ISP) / startup
// replay (RAW) -> acquire the event thread. The handle then stays open across
// sleep/wake; only the iso ring is started/stopped. This is the key to a clean
// restart without a physical replug (the OV580 degrades if the handle is closed
// + reopened, or the streaming alt-setting is toggled, between sessions).
bool Ps4CaptureSource::EnsureOpen()
{
    if (_open)
        return true;
    if (!UsbContext::Instance().Get())
        return false;
    if (!EnsureRunningDevice())
        return false;

    // Synchronous bring-up BEFORE acquiring the async event thread (libusb's sync
    // control transfers self-pump). We do NOT libusb_reset_device: a reset perturbs
    // the clean post-firmware state.
    libusb_set_configuration(_h, 1);
    libusb_claim_interface(_h, 0);
    libusb_claim_interface(_h, 1);

    // The mic ADC MUST be configured here -- claimed, but still on alt 0.
    // Hardware-measured across four orderings, counting how many of the 72
    // AK5703 register writes the OV580 accepted:
    //
    //   before alt 1                      72/72   <- live audio
    //   after alt 1, before commit        34/72
    //   after commit                      36/72
    //   after commit + PU control burst   25/72
    //
    // So it is selecting the STREAMING ALT SETTING that makes the vendor
    // register channel unreliable, not the commit and not the control traffic
    // -- which is why moving this inside BringUpIsp() was not enough: by then
    // alt 1 is already selected. A short write count leaves the array
    // misconfigured and permanently silent, which is exactly how the
    // microphone looked like a hardware wall.
    Ak5703BringUp();

    libusb_set_interface_alt_setting(_h, 1, 1);   // streaming alt; held open hereafter

    if (_useIsp)
    {
        if (!BringUpIsp())   // UVC probe/commit (YUY2) + resolve entities + AE
        {
            OutputDebugStringW(L"[PSCam4Win] PS4: ISP bring-up (UVC commit) failed\n");
            CloseDevice();
            return false;
        }
    }
    else
    {
        FirmwareBlobs blobs = EnsureFirmware();  // startup.bin (firmware already loaded)
        if (!blobs.startup.empty())
            ReplayStartup(_h, blobs.startup);
    }

    UsbContext::Instance().Acquire();   // event thread, kept while the handle is open
    _ctxAcquired = true;
    _open = true;
    return true;
}

// Submit the iso ring (per wake). The device is already open + committed, so this
// just (re-)applies the live controls and starts the transfers.
bool Ps4CaptureSource::StartIso()
{
    if (_streaming)
        return true;
    _stream.clear();
    _frameStart = 0;
    _synced = false;
    _ispFid = -1;
    if (_useIsp)
        ApplyControls();   // re-assert exposure/gain on each wake
    // rows/second is mode-dependent, so convert the settle TIME to rows here.
    _micSettleRows = (uint32_t)((uint64_t)kMicSettleMs *
                                _geom.rows * _geom.fps / 1000);
    _micLastQpc = 0;                   // no gap across a deliberate restart
    ResetMicDsp();                     // filter + resampler follow the video mode
    _micDropouts.store(0, std::memory_order_relaxed);
    _cancelling.store(false, std::memory_order_release);
    SubmitIsoRing();
    if (_xfers.empty())
        return false;
    _streaming = true;
    return true;
}

// Cancel the iso ring (per sleep). Crucially KEEPS the handle, the claimed
// interfaces, alt 1, and the event thread — so the next StartIso resumes the same
// clean session instead of forcing a replug.
void Ps4CaptureSource::StopIso()
{
    if (_streaming)
    {
        _cancelling.store(true, std::memory_order_release);
        for (auto* t : _xfers)
            libusb_cancel_transfer(t);
        // Wait for every cancelled transfer's completion callback to run BEFORE
        // freeing anything: libusb owns a submitted transfer (and its buffer) until
        // the callback fires, so freeing one mid-flight is a use-after-free on the
        // event thread. The event thread is still pumping here (CloseDevice releases
        // it only AFTER StopIso) and HandleIso treats a cancelled/errored transfer as
        // terminal, so _xfersInFlight drains within an event-loop tick; the long cap
        // is only a safety valve against a wedged driver that never calls back.
        for (int i = 0; i < 2000 && _xfersInFlight.load(std::memory_order_acquire) != 0; ++i)
            SleepMs(5);
        _streaming = false;
    }
    // Free only once nothing is in flight. If a transfer never completed (a wedged
    // driver — unreachable above), leak the ring in place: that is far safer than
    // freeing memory libusb may still write to.
    if (_xfersInFlight.load(std::memory_order_acquire) == 0)
        FreeTransfers();
    else
        OutputDebugStringW(L"[PSCam4Win] PS4: iso transfer never completed -- leaking ring to avoid UAF\n");
    _stream.clear();
    _frameStart = 0;
    _synced = false;
    _ispFid = -1;
}

void Ps4CaptureSource::SubmitIsoRing()
{
    // If a previous StopIso leaked the ring (a wedged transfer never called
    // back), rebuilding here would free/reuse buffers libusb may still write to —
    // the exact use-after-free the leak avoided. Stay unstreamable (StartIso sees
    // _xfers empty and fails; the controller retries) until the callback finally
    // drains the old ring, then reclaim it below before building the new one.
    if (_xfersInFlight.load(std::memory_order_acquire) != 0)
        return;
    if (!_xfers.empty())
        FreeTransfers();

    libusb_device* dev = libusb_get_device(_h);
    int maxPkt = libusb_get_max_iso_packet_size(dev, kIsoEp);
    if (maxPkt <= 0)
        maxPkt = 48 * 1024;

    _xfers.clear();
    _xferBufs.assign(kIsoRing, {});
    for (int i = 0; i < kIsoRing; ++i)
    {
        libusb_transfer* t = libusb_alloc_transfer(kIsoPkts);
        if (!t)
            break;
        _xferBufs[i].resize((size_t)kIsoPkts * maxPkt);
        libusb_fill_iso_transfer(t, _h, kIsoEp, _xferBufs[i].data(),
                                 (int)_xferBufs[i].size(), kIsoPkts,
                                 &Ps4CaptureSource::OnIso, this, 1000);
        libusb_set_iso_packet_lengths(t, maxPkt);
        _xfersInFlight.fetch_add(1, std::memory_order_acq_rel);
        if (libusb_submit_transfer(t) != 0)
        {
            _xfersInFlight.fetch_sub(1, std::memory_order_acq_rel);
            libusb_free_transfer(t);
            break;
        }
        _xfers.push_back(t);
    }
}

// Full teardown — only on physical removal (RetireAbsent) or process exit, NOT on
// idle sleep. Stops iso, releases the event thread, releases the interfaces, and
// closes the handle.
void Ps4CaptureSource::CloseDevice()
{
    StopIso();
    if (_ctxAcquired)
    {
        UsbContext::Instance().Release();
        _ctxAcquired = false;
    }
    if (_h)
    {
        libusb_release_interface(_h, 0);
        libusb_release_interface(_h, 1);
        libusb_close(_h);
        _h = nullptr;
    }
    _open = false;
}

void Ps4CaptureSource::FreeTransfers()
{
    for (auto* t : _xfers)
        if (t) libusb_free_transfer(t);
    _xfers.clear();
    _xferBufs.clear();
    _xfersInFlight.store(0, std::memory_order_release);
}

void Ps4CaptureSource::OnIso(libusb_transfer* xfer)
{
    static_cast<Ps4CaptureSource*>(xfer->user_data)->HandleIso(xfer);
}

void Ps4CaptureSource::HandleIso(libusb_transfer* xfer)
{
    if (xfer->status == LIBUSB_TRANSFER_COMPLETED ||
        xfer->status == LIBUSB_TRANSFER_TIMED_OUT)
    {
        for (int i = 0; i < xfer->num_iso_packets; ++i)
        {
            const libusb_iso_packet_descriptor& p = xfer->iso_packet_desc[i];
            if (p.status != LIBUSB_TRANSFER_COMPLETED || p.actual_length == 0)
                continue;
            const uint8_t* d = libusb_get_iso_packet_buffer_simple(xfer, i);
            if (d)
                OnPayload(d, (int)p.actual_length);
        }
    }

    // Resubmit to keep the ring full ONLY on the normal completed/timed-out path
    // (and only while not cancelling). Any other status is terminal for THIS
    // transfer: CANCELLED (StopIso tearing the ring down), NO_DEVICE (unplug), or
    // ERROR/STALL/OVERFLOW (a wedged endpoint). Resubmitting on a hard error here
    // would spin the event thread at 100% and keep _xfersInFlight pinned, so StopIso
    // could never drain and would free a still-in-flight transfer. A terminal
    // transfer is left in _xfers (freed by StopIso/FreeTransfers); if every transfer
    // goes terminal the ring empties and the controller's stall path re-wakes it.
    const bool resubmit =
        !_cancelling.load(std::memory_order_acquire) &&
        (xfer->status == LIBUSB_TRANSFER_COMPLETED ||
         xfer->status == LIBUSB_TRANSFER_TIMED_OUT);
    if (resubmit && libusb_submit_transfer(xfer) == 0)
        return;
    _xfersInFlight.fetch_sub(1, std::memory_order_acq_rel);
}

// Payload depacketiser.
//
// Hardware framing (reverse-engineered live): each iso packet carries
// ONE of two payload headers, then video bytes:
//   * a 12-byte UVC-style header on most packets: d[0]==0x0C (bHeaderLength),
//     d[1]==bmHeaderInfo with PTS+SCR present (d[1]&0x0C)==0x0C, bit0 FID toggle,
//     bit1 EOF, then 4-byte PTS + 6-byte SCR;
//   * a 4-byte continuation header on the rest (d[0]!=0x0C): a pair of 16-bit
//     running counters, no FID/EOF.
// The device emits EOF every few packets — i.e. EOF delimits a SLICE, not a full
// sensor image; one image spans many slices. So we strip the matching header off
// every packet and accumulate the contiguous video bytes, cutting a frame when a
// full image's worth has arrived (kFrameBytes). Geometry (stride / per-eye band
// offsets) is the Phase-0 1280x800 layout, validated on hardware.
void Ps4CaptureSource::OnPayload(const uint8_t* d, int len)
{
    if (len <= 0)
        return;
    if (_useIsp)
    {
        OnPayloadIsp(d, len);
        return;
    }

    // Each packet carries a 12-byte UVC header (d[0]==0x0C, PTS+SCR) or a 4-byte
    // continuation header; strip the matching length and append the video bytes.
    const bool hdr12 = (d[0] == 0x0c) && len >= 12 && ((d[1] & 0x0c) == 0x0c);
    const int  strip = hdr12 ? 12 : 4;
    const uint8_t* payload = d + strip;
    const int      plen    = len - strip;
    if (plen <= 0)
        return;

    if (_stream.size() + (size_t)plen > kMaxStream)
    {
        // Runaway without ever syncing: drop everything and restart.
        _stream.clear();
        _frameStart = 0;
        _synced = false;
    }
    _stream.insert(_stream.end(), payload, payload + plen);
    ExtractFrames();
}

// (Re)acquire frame alignment in _stream. The reliable signature is the
// frame-boundary row whose validMask (byte 0) == 0x00, immediately followed by
// the validMask ramp (next row's byte 0 is small, 0x02..), and a MATCHING
// boundary exactly one frame later. (The per-row 0x88 0x88 only appears on a few
// warm-up rows, so it cannot be used for phase.) Sets _frameStart to the frame's
// first row and drops everything before it. Returns true on success.
bool Ps4CaptureSource::ResyncLocked()
{
    const uint8_t* b = _stream.data();
    const size_t   n = _stream.size();
    const size_t   frame = (size_t)kFrameBytes;
    if (n < frame + kCleanRow + 1)   // need ~one frame + a row to confirm the period
        return false;

    const size_t limit = n - frame - 1;
    for (size_t p = 0; p <= limit && p < frame; ++p)
    {
        if (b[p] != 0x00)               continue;   // boundary-row validMask
        if (b[p + kCleanRow] >= 0x40)   continue;   // followed by the warm-up ramp
        if (b[p + frame]      != 0x00)  continue;   // boundary again one frame later
        _frameStart = p + kCleanRow;                // first row of the next frame
        _stream.erase(_stream.begin(), _stream.begin() + _frameStart);
        _frameStart = 0;
        _synced = true;
        return true;
    }
    return false;
}

void Ps4CaptureSource::ExtractFrames()
{
    for (;;)
    {
        if (!_synced)
        {
            if (!ResyncLocked())
                return;   // need more data
        }
        if (_stream.size() < _frameStart + kFrameBytes)
            return;       // frame not complete yet

        // Validate alignment: this frame's last row must be the 0x00 boundary.
        // A dropped packet would shift us, so re-sync instead of publishing junk.
        const uint8_t* fr = _stream.data() + _frameStart;
        if (fr[(size_t)(kFrameRows - 1) * kCleanRow] != 0x00)
        {
            _synced = false;
            _stream.erase(_stream.begin(), _stream.begin() + _frameStart + kCleanRow);
            _frameStart = 0;
            continue;
        }

        ReassembleAndPublish(fr);
        _frameStart += kFrameBytes;

        // Keep the rolling buffer bounded.
        if (_frameStart >= kFrameBytes)
        {
            _stream.erase(_stream.begin(), _stream.begin() + _frameStart);
            _frameStart = 0;
        }
    }
}

void Ps4CaptureSource::ReassembleAndPublish(const uint8_t* frame)
{
    std::lock_guard<std::mutex> lk(_latchMutex);
    bool any = false;
    // The eye image occupies the first kEyeHeight rows of the frame; copy each
    // row's non-null band into the persistent per-eye plane (eyes time-multiplex,
    // so last-non-null wins and both planes stay current).
    for (uint32_t r = 0; r < kEyeHeight; ++r)
    {
        const uint8_t* base  = frame + (size_t)r * kCleanRow;
        const uint8_t* lband = base + kLeftOffset;
        const uint8_t* rband = base + kRightOffset;
        if (!BandIsNull(lband))
        {
            memcpy(_pubLeft.data() + (size_t)r * kBandBytes, lband, kBandBytes);
            any = true;
        }
        if (!BandIsNull(rband))
        {
            memcpy(_pubRight.data() + (size_t)r * kBandBytes, rband, kBandBytes);
            any = true;
        }
    }
    if (any)
    {
        ++_seq;
        _latchCv.notify_all();
    }
}

// ISP depacketiser. Each iso packet carries a 12-byte UVC header (d[0]==0x0C,
// PTS+SCR) or a 4-byte continuation header, then YUYV video. In ISP mode the
// UVC frame-id bit (d[1] bit0) toggles exactly once per full combined image
// (~5.57 MB between toggles, confirmed), so we accumulate the stripped video and
// cut a frame when the FID flips. A short accumulation (dropped packets) is
// dropped and we re-lock on the next toggle.
void Ps4CaptureSource::OnPayloadIsp(const uint8_t* d, int len)
{
    const bool hdr12 = (d[0] == 0x0c) && len >= 12 && ((d[1] & 0x0c) == 0x0c);
    const int  strip = hdr12 ? 12 : 4;

    if (hdr12)
    {
        const int fid = d[1] & 1;
        // bmHeaderInfo bit2 = PTS present, bit3 = SCR present -- both set on
        // this firmware, which is what the (d[1] & 0x0c) == 0x0c test above
        // already relies on. Take the FIRST timestamp of each frame; it is what
        // tells PublishAudio whether a frame was really dropped or merely late.
        if (!_ispFramePtsValid)
        {
            _ispFramePts = (uint32_t)d[2] | ((uint32_t)d[3] << 8) |
                           ((uint32_t)d[4] << 16) | ((uint32_t)d[5] << 24);
            _ispFramePtsValid = true;
        }
        if (_ispFid >= 0 && fid != _ispFid)        // frame boundary
        {
            if (_stream.size() >= _geom.FrameBytes())
                PublishIspFrame(_stream.data(), _ispFramePts, _ispFramePtsValid);
            _stream.clear();
            _ispFramePtsValid = false;             // next frame brings its own
        }
        _ispFid = fid;
    }

    if (strip >= len)
        return;
    if (_stream.size() + (size_t)(len - strip) > (size_t)_geom.FrameBytes() * 2)
        _stream.clear();                            // runaway without a FID toggle
    _stream.insert(_stream.end(), d + strip, d + len);
}

// Power up and configure the AK5703 mic ADC. Runs once per device session,
// before streaming starts, exactly where the reference driver does it. Cheap
// (72 single-register writes) and harmless if it fails — the video path does not
// depend on it, so a failure just means no audio.
bool Ps4CaptureSource::Ak5703BringUp()
{
    if (!_h || !_useIsp)
        return false;
    int ok = 0;
    for (int i = 0; i < kAk5703InitCount; ++i)
        if (RegWriteMulti(_h, &kAk5703Init[i], 1, kAk5703Subaddr, kAk5703WriteMode) >= 0)
            ++ok;

    // The reference table leaves both gain registers at 0xa8, which measures
    // about -50 dBFS of room tone -- usable only after a lot of makeup gain.
    // Override it with the user's setting while we are still at alt 0.
    WriteMicGain();

    // Read-back is logged but deliberately does NOT gate readiness: on this
    // hardware the 0xa5/0xa6 read path errors on all four
    // probe registers even in the ordering that demonstrably produces live
    // audio, so requiring it would switch off a working microphone. Reads
    // straight after the 72-write burst are simply not dependable here.
    static const struct { uint16_t reg; uint8_t want; } kProbe[] = {
        { 0x00, 0x0c }, { 0x10, 0x08 }, { 0x01, 0x09 }, { 0x02, 0xc0 },
    };
    int answered = 0;
    for (const auto& probe : kProbe)
    {
        uint8_t v = 0;
        if (RegRead(_h, probe.reg, kAk5703Subaddr, kAk5703ReadMode, v) >= 0 && v == probe.want)
            ++answered;
    }

    // The write count IS a measured discriminator, not an assumption: this
    // sequence lands 72/72 when it runs before the UVC commit, but only 36/72
    // after the commit and 25/72 after the commit plus a PU control burst --
    // the pipe wedges partway and the rest of the registers go nowhere. So a
    // short count means the array is misconfigured and will be silent.
    const bool good = (ok == kAk5703InitCount);
    _micReady.store(good, std::memory_order_relaxed);
    wchar_t msg[160];
    _snwprintf_s(msg, _TRUNCATE,
                 L"[PSCam4Win] PS4: mic ADC bring-up %s -- %d/%d writes, %d/4 read-backs\n",
                 good ? L"OK" : L"FAILED", ok, kAk5703InitCount, answered);
    OutputDebugStringW(msg);
    return good;
}

// Slider percent -> AK5703 gain register. 0x80..0xff is the span worth
// exposing: measured room tone runs -61 dBFS at 0x80 up to -21 dBFS at 0xff,
// and below 0x80 the array is inaudible. The step is ~0.38 dB (AKM's 0.375 dB
// convention), so 100 slider positions cover roughly 40 dB.
uint8_t Ps4CaptureSource::MicGainReg() const
{
    const uint32_t p = _micGainPct > 100 ? 100 : _micGainPct;
    return (uint8_t)(0x80 + p * (0xff - 0x80) / 100);
}

// Registers 0x07 (mics 0+1) and 0x17 (mics 2+3) -- established by driving each
// to 0xff in turn and watching which channels moved. 0x08 and
// 0x18 hold the same value in the reference table but change nothing.
void Ps4CaptureSource::WriteMicGain()
{
    if (!_h)
        return;
    const uint8_t g = MicGainReg();
    const ps4::Ak5703Reg regs[2] = { { 0x0007, g }, { 0x0017, g } };
    for (int i = 0; i < 2; ++i)
        RegWriteMulti(_h, &regs[i], 1, kAk5703Subaddr, kAk5703WriteMode);
}

bool Ps4CaptureSource::SetMicGain(uint32_t pct)
{
    std::lock_guard<std::mutex> lk(_stateMutex);
    if (pct > 100)
        pct = 100;
    if (pct == _micGainPct)
        return true;
    _micGainPct = pct;
    if (!_open || !_useIsp || !_h)
        return true;                  // the next bring-up will pick it up

    // Gain registers only land with interface 1 on alt 0, and a
    // live write at alt 1 is worse than useless: measured, the FIRST vendor
    // write lands and the SECOND stalls, which would leave mics 0+1 loud and
    // 2+3 quiet -- a worse outcome than not being live at all. So drop to alt 0,
    // write, and bring the stream back: the same cycle RequestGeometry runs for
    // a mode change, and about as quick.
    const bool wasStreaming = _streaming;
    if (wasStreaming)
        StopIso();
    libusb_set_interface_alt_setting(_h, 1, 0);
    WriteMicGain();
    libusb_set_interface_alt_setting(_h, 1, 1);
    if (!BringUpIsp())
        return false;
    return !wasStreaming || StartIso();
}

// Rebuild the mic DSP for the current geometry. Called whenever the stream
// (re)starts, because the delivered audio rate follows the video mode.
//
// The DSP is told the DELIVERED slot count, rows*8. Only eyeH*8 of those are
// real samples, but the decoder consumes every slot -- byte 0 of the row
// header claims to mark the padding and demonstrably does not -- so rows*fps IS
// the rate it sees, and resampling that down to kMicSampleRate turns 808 rows
// into 800 samples a frame, which is exactly the eyeH*8 content rate. Passing
// eyeH*8 here instead makes the resampler a pass-through and publishes a 1 %
// fast stream.
void Ps4CaptureSource::ResetMicDsp()
{
    _micDsp.Reset(_geom.rows * 8, _geom.fps, ps4mic::DspConfig());
    _micLastPtsValid = false;
    _micPtsPerFrame  = 0;
    _micRateT0       = 0;
    _micRateFrames   = 0;
    _micRateMilliHz.store(0, std::memory_order_relaxed);
}

// Extract one video frame's worth of microphone audio and publish it into the
// ring. Decode -> anti-alias -> 8:1 decimate -> resample -> high-pass, all in
// Ps4MicDsp.h; this function owns everything frame-shaped around it: the ADC
// settle window, drop concealment, the frame-head repair, the level meters and
// the output-rate estimate.
//
// Two things here are load-bearing and easy to break:
//   * every DELIVERED row is decoded, trailer included. Reading only the image
//     rows discards 8 rows a frame and butt-joins the waveform 60 times a
//     second.
//   * the frame HEAD is repaired, not trusted. The row header's valid mask ramps
//     across the first rows of every frame, the filter smears that over the
//     first ~14 outputs, measuring 2-5.7x the median step at a fixed offset.
//     Honouring the mask instead measures WORSE.
//
// Runs on the iso event thread: 808 rows x 4 channels x 64 taps is ~12 M
// multiply-accumulates per second at 60 fps, around 1 % of a core.
void Ps4CaptureSource::PublishAudio(const uint8_t* frame, uint32_t pts, bool ptsValid)
{
    if (!_micReady.load(std::memory_order_relaxed))
        return;
    const uint32_t rowBytes = _geom.RowBytes();
    const uint32_t rows     = _geom.rows;          // ALL delivered rows carry audio

    // The ADC takes about 400 ms to settle after the stream starts, and it does
    // so LOUDLY: measured per-100 ms window RMS from stream start is
    // -20, -28, -39, -48, then the room's actual -52 dBFS. Passed through, that
    // is a thump at the head of every recording and a meter that slams to full
    // scale before it means anything. Discard it rather than publish it.
    // firstRow, not just a counter: on the frame where the settle window runs
    // out partway through, the head of that frame must be skipped too.
    uint32_t firstRow = 0;
    if (_micSettleRows)
    {
        const uint32_t skip = _micSettleRows < rows ? _micSettleRows : rows;
        _micSettleRows -= skip;
        if (skip == rows)
            return;                                 // whole frame inside the settle
        firstRow = skip;
    }
    int64_t sumSq[kMicChannels] = {};

    std::lock_guard<std::mutex> lk(_micMutex);
    if (_micRing.size() != (size_t)kMicRingFrames * kMicChannels)
        _micRing.assign((size_t)kMicRingFrames * kMicChannels, 0);

    // ---- conceal gaps left by dropped video frames ------------------------
    // The microphone rides inside the video stream, so a dropped frame removes
    // a frame's worth of audio. Publishing the next frame straight after the
    // previous one BUTT-JOINS the waveform, and a step discontinuity mid-vowel
    // is heard as a click. Ramp down, hold silence for the real duration, and
    // ramp the next frame in.
    //
    // The DEVICE's presentation timestamp decides, not the host clock. The old
    // rule -- lost = (int)(elapsed*fps - 1.0 + 0.5) on QPC -- is honest at 60
    // and 120 fps but trips on ordinary iso jitter at 240, where one frame is
    // 4.2 ms it conceals twice against a single real gap, inventing 200 samples
    // of audio that was never missing. PTS cannot make
    // that mistake, because a frame that merely arrived late still carries its
    // own timestamp. QPC remains only as the fallback if PTS ever goes missing.
    uint32_t fadeIn = 0;
    int lost = 0;
    if (ptsValid && _micLastPtsValid)
    {
        const uint32_t delta = pts - _micLastPts;          // wraps correctly
        // The frame period is the SMALLEST delta we have seen: a real period
        // cannot be shorter than nominal, while a drop only ever makes one
        // longer. Seeded from the first delta and only ever revised down.
        if (delta && (!_micPtsPerFrame || delta < _micPtsPerFrame))
            _micPtsPerFrame = delta;
        if (_micPtsPerFrame)
            lost = (int)((delta + _micPtsPerFrame / 2) / _micPtsPerFrame) - 1;
    }
    else
    {
        if (!_micQpcFreq)
        {
            LARGE_INTEGER f{};
            QueryPerformanceFrequency(&f);
            _micQpcFreq = f.QuadPart;
        }
        LARGE_INTEGER nowQpc{};
        QueryPerformanceCounter(&nowQpc);
        if (_micLastQpc && _micQpcFreq && _geom.fps)
        {
            const double elapsed = double(nowQpc.QuadPart - _micLastQpc) / double(_micQpcFreq);
            lost = (int)(elapsed * _geom.fps - 1.0 + 0.5);
        }
        _micLastQpc = nowQpc.QuadPart;
    }
    if (ptsValid) { _micLastPts = pts; _micLastPtsValid = true; }

    const uint32_t kRamp = 96;                       // ~2 ms at 48 kHz
    if (lost > 0 && lost <= (int)kMicMaxConceal)
    {
        // OUTPUT samples, not rows: the ring runs at kMicSampleRate while a
        // frame delivers `rows` rows at the higher pre-resample rate.
        const uint32_t missing =
            (uint32_t)lost * (kMicSampleRate / (_geom.fps ? _geom.fps : 60));
        int16_t last[kMicChannels] = {};
        if (_micWritePos > 0)
        {
            const size_t prev = (size_t)((_micWritePos - 1) % kMicRingFrames) * kMicChannels;
            for (uint32_t c = 0; c < kMicChannels; ++c)
                last[c] = _micRing[prev + c];
        }
        const uint32_t ramp = missing < kRamp ? missing : kRamp;
        for (uint32_t i = 0; i < missing; ++i)
        {
            const size_t slot = (size_t)(_micWritePos % kMicRingFrames) * kMicChannels;
            for (uint32_t c = 0; c < kMicChannels; ++c)
                _micRing[slot + c] = (i < ramp)
                    ? (int16_t)((int)last[c] * (int)(ramp - i) / (int)ramp) : 0;
            ++_micWritePos;
        }
        _micDropouts.fetch_add(1, std::memory_order_relaxed);
        fadeIn = kRamp;
    }

    // ---- decode + filter + resample --------------------------------------
    // Collected per FRAME rather than streamed straight into the ring, because
    // the frame-head repair below needs the samples either side of its window.
    // One reused buffer, so no allocation on the iso thread.
    uint32_t emitted = 0;
    int16_t  out[ps4mic::MicDsp::kMaxEmit * kMicChannels];
    if (_micFrameBuf.size() < (size_t)(rows + 16) * kMicChannels)
        _micFrameBuf.assign((size_t)(rows + 16) * kMicChannels, 0);
    for (uint32_t r = firstRow; r < rows; ++r)
    {
        // row[0] is the per-slot valid mask; row[32..95] is the audio field.
        const uint8_t* row = frame + (size_t)r * rowBytes;
        const uint32_t got = _micDsp.PushRowBytes(row + 32, out, row[0]);
        for (uint32_t e = 0; e < got && emitted * kMicChannels < _micFrameBuf.size(); ++e)
        {
            for (uint32_t c = 0; c < kMicChannels; ++c)
                _micFrameBuf[(size_t)emitted * kMicChannels + c] =
                    out[(size_t)e * kMicChannels + c];
            ++emitted;
        }
    }

    // Bridge the frame-head disturbance before anything sees it. Only on a
    // whole frame: a partial one (the settle boundary) has no head to repair.
    if (firstRow == 0)
        ps4mic::MicDsp::RepairFrameHead(_micFrameBuf.data(), emitted);

    for (uint32_t i = 0; i < emitted; ++i)
    {
        const size_t slot = (size_t)(_micWritePos % kMicRingFrames) * kMicChannels;
        for (uint32_t c = 0; c < kMicChannels; ++c)
        {
            int32_t v = _micFrameBuf[(size_t)i * kMicChannels + c];
            if (fadeIn && i < fadeIn)
                v = (int32_t)((int64_t)v * i / fadeIn);   // ramp in after a gap
            _micRing[slot + c] = (int16_t)v;
            // Accumulate at FULL precision. The old form squared (v >> 8),
            // which is 0 for every sample under 256 LSB -- so a -50 dBFS room
            // tone read a fixed ~-45 dBFS floor and -70 read the same. These
            // meters are also the instrument the array is judged with.
            sumSq[c] += (int64_t)v * v;
        }
        ++_micWritePos;
    }
    // Drop the oldest audio if nobody is draining the ring.
    if (_micWritePos - _micReadPos > kMicRingFrames)
        _micReadPos = _micWritePos - kMicRingFrames;

    // Measured output rate. The resampler TARGETS kMicSampleRate, but it is fed
    // the nominal fps while the camera runs at 60.029, so what actually leaves
    // is ~48,023 Hz here and differs per mode. Anything that has to line this
    // stream up against real time -- a file's header, a live render into a
    // fixed-rate endpoint -- needs the truth rather than the target.
    if (emitted)
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (!_micQpcFreq)
        {
            LARGE_INTEGER f{}; QueryPerformanceFrequency(&f);
            _micQpcFreq = f.QuadPart;
        }
        // A SLIDING window, restarted whenever the measurement is invalidated.
        // A cumulative average since stream start cannot recover: one stall
        // skews it permanently and it only crawls back asymptotically, which is
        // exactly what was observed (a brief capture interruption took the
        // estimate from +487 ppm to -572 and it was still wrong a minute later).
        //
        // Two things invalidate it:
        //   * a CONCEALED GAP -- frames were missing, so the sample count no
        //     longer matches elapsed time and the ratio is meaningless;
        //   * age -- past ~20 s the window is answering a stale question.
        const uint32_t drops = _micDropouts.load(std::memory_order_relaxed);
        const double secs = _micRateT0
            ? double(now.QuadPart - _micRateT0) / double(_micQpcFreq) : 0.0;
        if (!_micRateT0 || drops != _micRateDrops || secs > 20.0)
        {
            _micRateT0     = now.QuadPart;
            _micRateFrames = 0;
            _micRateDrops  = drops;
        }
        else
        {
            _micRateFrames += emitted;
            if (secs > 3.0)          // under a few seconds it is mostly jitter
                _micRateMilliHz.store((uint32_t)(_micRateFrames * 1000.0 / secs + 0.5),
                                      std::memory_order_relaxed);
        }
    }

    for (uint32_t c = 0; c < kMicChannels; ++c)
    {
        const double rms = emitted
            ? sqrt((double)sumSq[c] / (double)emitted) / 32768.0 : 0.0;
        _micLevel[c].store((uint32_t)(rms * 10000.0 < 10000.0 ? rms * 10000.0 : 10000.0),
                           std::memory_order_relaxed);
    }
}

uint32_t Ps4CaptureSource::ReadAudio(int16_t* dst, uint32_t frames)
{
    if (!dst || !frames)
        return 0;
    std::lock_guard<std::mutex> lk(_micMutex);
    if (_micRing.empty())
        return 0;
    uint64_t avail = _micWritePos - _micReadPos;
    if (avail > kMicRingFrames) avail = kMicRingFrames;
    const uint32_t n = (uint32_t)(avail < frames ? avail : frames);
    for (uint32_t i = 0; i < n; ++i)
    {
        const size_t slot = (size_t)((_micReadPos + i) % kMicRingFrames) * kMicChannels;
        for (uint32_t c = 0; c < kMicChannels; ++c)
            dst[(size_t)i * kMicChannels + c] = _micRing[slot + c];
    }
    _micReadPos += n;
    return n;
}

bool Ps4CaptureSource::AudioLevels(float out[kMicChannels]) const
{
    if (!_micReady.load(std::memory_order_relaxed))
        return false;
    for (uint32_t c = 0; c < kMicChannels; ++c)
        out[c] = _micLevel[c].load(std::memory_order_relaxed) / 10000.0f;
    return true;
}

// Split one combined YUYV frame into the left/right YUY2 eye planes. Both eyes
// are always present in ISP mode, so both planes refresh every frame.
void Ps4CaptureSource::PublishIspFrame(const uint8_t* frame, uint32_t pts, bool ptsValid)
{
    {
        std::lock_guard<std::mutex> lk(_latchMutex);
        const uint32_t rowBytes  = _geom.RowBytes();
        const uint32_t bandBytes = _geom.BandBytes();
        const uint32_t leftOff   = _geom.LeftOff();
        const uint32_t rightOff  = _geom.RightOff();
        for (uint32_t r = 0; r < _geom.eyeH; ++r)
        {
            const uint8_t* base = frame + (size_t)r * rowBytes;
            memcpy(_pubLeft.data()  + (size_t)r * bandBytes, base + leftOff,  bandBytes);
            memcpy(_pubRight.data() + (size_t)r * bandBytes, base + rightOff, bandBytes);
        }
        ++_seq;
        _latchCv.notify_all();
    }

    // Audio OUTSIDE the video latch: it is ~207k MACs per frame and would
    // otherwise block every SnapshotPlanes and WaitNewer. Both calls run on the
    // iso event thread, so the split changes no ordering.
    PublishAudio(frame, pts, ptsValid);
}

uint64_t Ps4CaptureSource::WaitNewer(uint64_t lastSeq, uint32_t timeoutMs)
{
    std::unique_lock<std::mutex> lk(_latchMutex);
    _latchCv.wait_for(lk, std::chrono::milliseconds(timeoutMs),
                      [&] { return _seq != lastSeq; });
    return _seq;
}

uint64_t Ps4CaptureSource::SnapshotPlanes(uint8_t* leftDst, uint8_t* rightDst)
{
    // _eyeBytes, not _geom.EyeBytes(): this runs on a view's capture thread.
    // See the _eyeBytes declaration for why an atomic rather than _stateMutex.
    const uint32_t bytes = _eyeBytes.load(std::memory_order_acquire);
    std::lock_guard<std::mutex> lk(_latchMutex);
    if (_seq == 0)
        return 0;
    if (leftDst)
        memcpy(leftDst, _pubLeft.data(), bytes);
    if (rightDst)
        memcpy(rightDst, _pubRight.data(), bytes);
    return _seq;
}

} // namespace ps4
