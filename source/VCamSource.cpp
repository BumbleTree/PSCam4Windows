//
// MediaSource / MediaStream implementation. See VCamSource.h for the overview.
//
#include "VCamSource.h"
#include <cstdarg>
#include <cstdio>

void VCamTrace(const wchar_t* fmt, ...)
{
    wchar_t buf[512];
    va_list args;
    va_start(args, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, args);
    va_end(args);
    wchar_t line[560];
    _snwprintf_s(line, _TRUNCATE, L"PSCam4Win: %s\n", buf);
    OutputDebugStringW(line);
}

// ===========================================================================
// MediaStream
// ===========================================================================

MediaStream::MediaStream(MediaSource* parent, int cameraIndex) : _parent(parent), _cameraIndex(cameraIndex)
{
    DllAddRef();
}

namespace
{

void FillYuy2Black(uint8_t* dst, uint32_t w, uint32_t h)
{
    yuv::FillBlack(dst, w, h);   // buffers here are exactly w*h*2
}

} // namespace

HRESULT MediaStream::Initialize()
{
    // The host publishes the FrameBus before registering the virtual camera,
    // so normally the real capture format is already known here. The fallback
    // only triggers if the source is activated while the host is not running.
    framebus::HotHeader fmt{};
    uint32_t defaultWidth = 640, defaultHeight = 480;
    uint32_t defaultFpsNum = 60, defaultFpsDen = 1;
    bool busFormatKnown = false;
    if (_bus.TryOpen(_cameraIndex) && _bus.ReadFormat(fmt))
    {
        defaultWidth = fmt.width;
        defaultHeight = fmt.height;
        defaultFpsNum = fmt.fpsNum;
        defaultFpsDen = fmt.fpsDen;
        busFormatKnown = true;
        VCamTrace(L"stream: FrameBus default format %ux%u @ %u/%u", defaultWidth, defaultHeight, defaultFpsNum, defaultFpsDen);
    }
    else
    {
        VCamTrace(L"stream: FrameBus not available, defaulting to 640x480@60");
    }

    _width = defaultWidth;
    _height = defaultHeight;
    _fpsNum = defaultFpsNum;
    _fpsDen = defaultFpsDen;
    _subtype = MFVideoFormat_NV12;
    _frameBytes = framebus::Nv12Bytes(_width, _height);
    _frameDuration = MulDiv(10000000, _fpsDen, _fpsNum);

    // _staging always holds the frame in YUY2 (the bus/native format) at the
    // negotiated size; NV12 clients are converted at delivery time.
    _staging.reset(new (std::nothrow) uint8_t[framebus::Yuy2Bytes(_width, _height)]);
    if (!_staging)
        return E_OUTOFMEMORY;
    FillBlack();

    HRESULT hr = MFCreateEventQueue(&_queue);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&_attributes, 4);
    if (FAILED(hr)) return hr;
    _attributes->SetGUID(MF_DEVICESTREAM_STREAM_CATEGORY, kPinCategoryCapture);
    _attributes->SetUINT32(MF_DEVICESTREAM_STREAM_ID, 0);
    _attributes->SetUINT32(MF_DEVICESTREAM_FRAMESERVER_SHARED, 1);
    _attributes->SetUINT32(MF_DEVICESTREAM_ATTRIBUTE_FRAMESOURCE_TYPES, kFrameSourceTypeColor);

    // Advertised media types come from the FrameBus capability block (ColdBlock),
    // NOT a hardcoded mode table, so each device advertises exactly its own
    // modes/formats. The host writes the ColdBlock before registering the virtual
    // camera, so it is present here whenever the camera is live; if it
    // is absent we fail rather than leak another device's defaults.
    framebus::ColdBlock cold{};
    if (!(_bus.IsOpen() && _bus.ReadColdBlock(cold)))
    {
        VCamTrace(L"stream: FrameBus ColdBlock unavailable -- cannot build media types");
        return E_FAIL;
    }

    // Scratch for a bus frame that differs from the negotiated size (the scaling
    // paths in DeliverSample). Sized from this slot's largest advertised mode,
    // hence allocated after the ColdBlock read. kAbsMaxFrameBytes remains the
    // ceiling: the ColdBlock is shared memory, so a corrupt modeCount must not
    // size an allocation.
    {
        uint32_t maxBusBytes = framebus::Yuy2Bytes(defaultWidth, defaultHeight);
        for (uint32_t i = 0; i < cold.modeCount && i < framebus::kMaxModes; ++i)
        {
            const uint32_t bytes = framebus::Yuy2Bytes(cold.modes[i].width, cold.modes[i].height);
            if (bytes > maxBusBytes)
                maxBusBytes = bytes;
        }
        if (maxBusBytes > framebus::kAbsMaxFrameBytes)
            maxBusBytes = framebus::kAbsMaxFrameBytes;
        _busStagingBytes = maxBusBytes;
        _busStaging.reset(new (std::nothrow) uint8_t[maxBusBytes]);
        if (!_busStaging)
            return E_OUTOFMEMORY;
    }

    // Per-transport default subtype (NV12 for PS3 Eye, YUY2 for EyeToy).
    const GUID defaultSubtype =
        (cold.defaultFormat == framebus::kFmtYUY2)  ? MFVideoFormat_YUY2 :
        (cold.defaultFormat == framebus::kFmtMJPEG) ? MFVideoFormat_MJPG :
                                                      MFVideoFormat_NV12;

    // Fixed emit order so an unchanged device (PS3 Eye, mask YUY2|NV12) advertises
    // the identical NV12-then-YUY2 ordering it always has; only formats present in
    // the mode's mask are created.
    struct FmtMap { uint32_t bit; GUID subtype; };
    static const FmtMap kFmts[] = {
        { framebus::kFmtNV12,  MFVideoFormat_NV12 },
        { framebus::kFmtYUY2,  MFVideoFormat_YUY2 },
        { framebus::kFmtMJPEG, MFVideoFormat_MJPG },
    };

    // ComPtr: ownership follows the vector, so an early return cannot leak.
    std::vector<ComPtr<IMFMediaType>> typesList;
    ComPtr<IMFMediaType> defaultType;
    bool advertisesMjpeg = false;

    // A raw (uncompressed) type may only be advertised at a size DeliverSample
    // can actually PRODUCE from the current bus frame: the bus size itself, or
    // the single scaled pair the DLL implements (320x240 <-> 640x480). Every
    // other size falls into DeliverSample's `else` branch and is filled with
    // FillYuy2Black — so advertising it hands the client a media type that can
    // only ever show black, at full frame rate, with nothing to indicate why.
    //
    // Advertise only what the pipeline can deliver at the CONFIGURED camera mode
    // and let the Frame Server synthesize the rest. The rule lives in
    // yuv::ScalePlanFor so DeliverSample decides with the same code.
    auto RawSizeDeliverable = [&](uint32_t w, uint32_t h) -> bool
    {
        if (!busFormatKnown)
            return true;    // no bus yet — advertise the profile and re-run later
        return yuv::ScalePlanFor(defaultWidth, defaultHeight, w, h) != yuv::ScalePlan::Unsupported;
    };

    for (uint32_t i = 0; i < cold.modeCount; ++i)
    {
        const framebus::ColdMode& m = cold.modes[i];
        for (const FmtMap& f : kFmts)
        {
            if (!(m.formatMask & f.bit))
                continue;
            // MJPEG is passthrough-only: the sidecar carries JFIF at the
            // camera's CONFIGURED size and a JPEG cannot be rescaled without
            // decoding, so an MJPG type at any other size can never deliver
            // (DeliverSample's dims guard would starve it into ~5fps stale
            // re-deliveries — and the Frame Server backs its synthesized NV12
            // types with the MJPG native, starving those clients too). Only
            // advertise MJPG at the current bus dimensions. Preset changes are
            // applied between sessions via a re-register, which re-runs this.
            if (f.subtype == MFVideoFormat_MJPG &&
                !(busFormatKnown && m.width == defaultWidth && m.height == defaultHeight))
                continue;
            if (f.subtype != MFVideoFormat_MJPG && !RawSizeDeliverable(m.width, m.height))
                continue;
            ComPtr<IMFMediaType> mt;
            if (FAILED(CreateMediaType(m.width, m.height, m.fps, 1, f.subtype, &mt)))
                continue;
            typesList.push_back(mt);
            if (f.subtype == MFVideoFormat_MJPG)
                advertisesMjpeg = true;

            if (!defaultType &&
                m.width == defaultWidth && m.height == defaultHeight &&
                m.fps == defaultFpsNum && f.subtype == defaultSubtype)
            {
                defaultType = mt;
            }
        }
    }

    if (typesList.empty())
        return E_FAIL;

    // MJPEG passthrough needs a JFIF scratch buffer; allocate it only when an
    // MJPG type actually made the advertised list (PS3 Eye never has one, and a
    // bus-less fallback session advertises none, so both pay nothing).
    if (advertisesMjpeg)
    {
        _jpegStaging.reset(new (std::nothrow) uint8_t[framebus::kMaxJpegBytes]);
        if (!_jpegStaging)
            return E_OUTOFMEMORY;
    }

    if (!defaultType)
    {
        defaultType = typesList[0];
    }

    // MFCreateStreamDescriptor takes a raw array; borrow the pointers for the
    // call only. The ComPtrs in typesList keep every type alive across it, and
    // the descriptor takes its own references.
    std::vector<IMFMediaType*> rawTypes;
    rawTypes.reserve(typesList.size());
    for (const auto& mt : typesList)
        rawTypes.push_back(mt.Get());

    hr = MFCreateStreamDescriptor(0, static_cast<DWORD>(rawTypes.size()), rawTypes.data(), &_descriptor);
    if (FAILED(hr)) return hr;

    ComPtr<IMFMediaTypeHandler> handler;
    hr = _descriptor->GetMediaTypeHandler(&handler);
    if (FAILED(hr)) return hr;
    hr = handler->SetCurrentMediaType(defaultType.Get());
    if (FAILED(hr)) return hr;

    _parentRef = static_cast<IMFMediaSourceEx*>(_parent);
    return S_OK;
}

HRESULT MediaStream::CreateMediaType(uint32_t width, uint32_t height, uint32_t fpsNum, uint32_t fpsDen, GUID subtype, IMFMediaType** ppType)
{
    ComPtr<IMFMediaType> mt;
    HRESULT hr = MFCreateMediaType(&mt);
    if (FAILED(hr)) return hr;

    mt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    mt->SetGUID(MF_MT_SUBTYPE, subtype);
    MFSetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, width, height);
    MFSetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, fpsNum, fpsDen);
    MFSetAttributeRatio(mt.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    mt->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    mt->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);

    if (subtype == MFVideoFormat_NV12)
    {
        mt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        mt->SetUINT32(MF_MT_DEFAULT_STRIDE, width);
        mt->SetUINT32(MF_MT_SAMPLE_SIZE, framebus::Nv12Bytes(width, height));
    }
    else if (subtype == MFVideoFormat_YUY2)
    {
        mt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, TRUE);
        mt->SetUINT32(MF_MT_DEFAULT_STRIDE, width * 2);
        mt->SetUINT32(MF_MT_SAMPLE_SIZE, width * height * 2);
    }
    else if (subtype == MFVideoFormat_MJPG)
    {
        // Compressed, variable-size: each JFIF frame is an independent keyframe.
        // No fixed sample size and no stride; advertise the sidecar capacity as
        // the max buffer hint.
        mt->SetUINT32(MF_MT_FIXED_SIZE_SAMPLES, FALSE);
        mt->SetUINT32(MF_MT_SAMPLE_SIZE, framebus::kMaxJpegBytes);
    }

    *ppType = mt.Detach();
    return S_OK;
}

void MediaStream::FillBlack()
{
    FillYuy2Black(_staging.get(), _width, _height);
}

// Stamp the keepalive (and optionally pulse the wake event) so the host knows
// a client is consuming frames. Caller holds _lock. No-ops while the host's
// control objects don't exist; retries opening at most every 500ms.
void MediaStream::PingActivity(bool forceWakeSignal)
{
    const ULONGLONG now = GetTickCount64();
    if (!_ping.IsOpen())
    {
        if (now < _nextPingRetryTick)
            return;
        _nextPingRetryTick = now + 500;
        if (!_ping.TryOpen(_cameraIndex))
            return;
    }
    _ping.Stamp();                          // always tick BEFORE event
    _ping.SetConsumerMask(_consumerMask);   // re-assert after any (re)open
    if (forceWakeSignal || now - _lastWakeSignalTick >= 1000)
    {
        _lastWakeSignalTick = now;
        _ping.SignalWake();
    }
}

// Maps a negotiated MF subtype to the ControlBus consumer bit the host reads to
// gate optional per-frame work (e.g. the JPEG sidecar). Unknown subtypes map to
// 0, which the host treats as "decode YUY2, skip JPEG".
static uint32_t ConsumerBitForSubtype(const GUID& subtype)
{
    if (subtype == MFVideoFormat_YUY2) return controlbus::kConsumeYUY2;
    if (subtype == MFVideoFormat_NV12) return controlbus::kConsumeNV12;
    if (subtype == MFVideoFormat_MJPG) return controlbus::kConsumeMJPEG;
    return 0;
}

HRESULT MediaStream::Start(const PROPVARIANT* startPosition)
{
    AutoLock lock(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;

    // Stage everything fallible BEFORE committing any live state: a stream
    // marked RUNNING with a null staging buffer would fault inside the Frame
    // Server on the next RequestSample.
    uint32_t width = _width, height = _height, fpsNum = _fpsNum, fpsDen = _fpsDen;
    GUID subtype = _subtype;

    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType> currentType;
    if (SUCCEEDED(_descriptor->GetMediaTypeHandler(&handler)) &&
        SUCCEEDED(handler->GetCurrentMediaType(&currentType)))
    {
        MFGetAttributeSize(currentType.Get(), MF_MT_FRAME_SIZE, &width, &height);
        MFGetAttributeRatio(currentType.Get(), MF_MT_FRAME_RATE, &fpsNum, &fpsDen);
        subtype = MFVideoFormat_NV12;
        currentType->GetGUID(MF_MT_SUBTYPE, &subtype);
    }

    std::shared_ptr<uint8_t[]> staging(new (std::nothrow) uint8_t[framebus::Yuy2Bytes(width, height)]);
    if (!staging)
        return E_OUTOFMEMORY;
    FillYuy2Black(staging.get(), width, height);

    _width = width;
    _height = height;
    _fpsNum = fpsNum;
    _fpsDen = fpsDen;
    _subtype = subtype;
    _consumerMask = ConsumerBitForSubtype(subtype);  // host gates JPEG sidecar on this
    _frameBytes = (subtype == MFVideoFormat_YUY2) ? framebus::Yuy2Bytes(width, height)
                                                  : framebus::Nv12Bytes(width, height);
    _frameDuration = MulDiv(10000000, fpsDen, fpsNum);
    _staging = std::move(staging);
    _state = MF_STREAM_STATE_RUNNING;
    _lastFrameId = 0;  // re-sync with whatever the bus currently holds

    VCamTrace(L"stream: started with format %ux%u @ %u/%u (subtype YUY2=%d)",
              width, height, fpsNum, fpsDen, (subtype == MFVideoFormat_YUY2));
    PingActivity(true);  // publishes _consumerMask alongside the keepalive
    return _queue->QueueEventParamVar(MEStreamStarted, GUID_NULL, S_OK, startPosition);
}

HRESULT MediaStream::Stop()
{
    AutoLock lock(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    _state = MF_STREAM_STATE_STOPPED;
    _consumerMask = 0;
    _ping.SetConsumerMask(0);               // no active client consuming this format
    VCamTrace(L"stream: stopped");
    return _queue->QueueEventParamVar(MEStreamStopped, GUID_NULL, S_OK, nullptr);
}

HRESULT MediaStream::Shutdown()
{
    AutoLock lock(_lock);
    if (_shutdown)
        return S_OK;
    _shutdown = true;
    _state = MF_STREAM_STATE_STOPPED;
    _consumerMask = 0;
    _ping.SetConsumerMask(0);               // client gone; stop gating work on it
    if (_queue)
        _queue->Shutdown();
    // _bus is intentionally NOT closed here: a DeliverSample call on another
    // thread may still be polling the mapped view. The destructor closes it
    // once no method can be executing.
    _descriptor.Reset();
    _attributes.Reset();
    _parentRef.Reset();   // break the source<->stream reference cycle
    _parent = nullptr;
    VCamTrace(L"stream: shutdown");
    return S_OK;
}

// ----- IUnknown -------------------------------------------------------------

STDMETHODIMP MediaStream::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaStream2) ||
        riid == __uuidof(IMFMediaStream) || riid == __uuidof(IMFMediaEventGenerator))
        *ppv = static_cast<IMFMediaStream2*>(this);
    else if (riid == __uuidof(IKsControl))
        *ppv = static_cast<IKsControl*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaStream::AddRef()
{
    return InterlockedIncrement(&_refCount);
}

STDMETHODIMP_(ULONG) MediaStream::Release()
{
    const ULONG ref = InterlockedDecrement(&_refCount);
    if (ref == 0)
    {
        delete this;
        DllRelease();
    }
    return ref;
}

// ----- IMFMediaEventGenerator ------------------------------------------------

STDMETHODIMP MediaStream::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    AutoLock lock(_lock);
    if (_shutdown) return MF_E_SHUTDOWN;
    return _queue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP MediaStream::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    AutoLock lock(_lock);
    if (_shutdown) return MF_E_SHUTDOWN;
    return _queue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP MediaStream::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    // Never hold the object lock across the (potentially blocking) GetEvent.
    ComPtr<IMFMediaEventQueue> queue;
    {
        AutoLock lock(_lock);
        if (_shutdown) return MF_E_SHUTDOWN;
        queue = _queue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP MediaStream::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                     HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    AutoLock lock(_lock);
    if (_shutdown) return MF_E_SHUTDOWN;
    return _queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// ----- IMFMediaStream ---------------------------------------------------------

STDMETHODIMP MediaStream::GetMediaSource(IMFMediaSource** ppMediaSource)
{
    if (!ppMediaSource)
        return E_POINTER;
    AutoLock lock(_lock);
    if (_shutdown || !_parentRef)
        return MF_E_SHUTDOWN;
    *ppMediaSource = _parentRef.Get();
    (*ppMediaSource)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaStream::GetStreamDescriptor(IMFStreamDescriptor** ppStreamDescriptor)
{
    if (!ppStreamDescriptor)
        return E_POINTER;
    AutoLock lock(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    *ppStreamDescriptor = _descriptor.Get();
    (*ppStreamDescriptor)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaStream::RequestSample(IUnknown* pToken)
{
    {
        AutoLock lock(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        if (_state != MF_STREAM_STATE_RUNNING)
            return MF_E_MEDIA_SOURCE_WRONGSTATE;
        PingActivity(false);
    }
    return DeliverSample(pToken);
}

HRESULT MediaStream::DeliverSample(IUnknown* token)
{
    // Serialize deliveries: the staging buffers below belong to exactly one
    // in-flight DeliverSample at a time. _deliverLock is never taken by any
    // other method, so Start/Stop/Shutdown can't block on a delivery.
    AutoLock deliverLock(_deliverLock);

    // Snapshot the negotiated format and the staging buffer under the object
    // lock. Start() may renegotiate (swap _staging, rewrite the dimensions)
    // concurrently; working from a consistent snapshot prevents both torn
    // reads and a use-after-free of the buffer (the shared_ptr keeps the old
    // buffer alive until this call finishes with it).
    GUID     subtype;
    uint32_t width, height, frameBytes;
    LONGLONG frameDuration;
    std::shared_ptr<uint8_t[]> staging;
    {
        AutoLock lock(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        if (_state != MF_STREAM_STATE_RUNNING)
            return MF_E_MEDIA_SOURCE_WRONGSTATE;
        subtype       = _subtype;
        width         = _width;
        height        = _height;
        frameBytes    = _frameBytes;
        frameDuration = _frameDuration;
        staging       = _staging;
    }

    // MJPEG clients (EyeToy) get the source JFIF passed through verbatim; YUY2/
    // NV12 clients get the decoded/converted frame. _jpegStaging is null unless
    // the device advertises MJPEG, so PS3 never enters the passthrough path.
    const bool isMjpeg = (subtype == MFVideoFormat_MJPG);

    // Pace delivery to the camera: wait for a frame newer than the last one
    // we handed out, sleeping on the writer's frame-ready event between
    // attempts (Sleep-based polling was quantized to the 15.6ms system timer
    // and silently capped the 100..187fps modes). On deadline we re-deliver
    // the previous frame so the pipeline keeps flowing even if the host
    // stalls or exits.
    const DWORD periodMs = static_cast<DWORD>(frameDuration / 10000);
    const ULONGLONG deadline = GetTickCount64() + periodMs * 2 + 50;

    for (;;)
    {
        {
            AutoLock lock(_lock);
            if (_shutdown)
                return MF_E_SHUTDOWN;
            if (_state != MF_STREAM_STATE_RUNNING)
                return MF_E_MEDIA_SOURCE_WRONGSTATE;
        }
        if (!_bus.IsOpen())
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= _nextBusRetryTick)
            {
                _nextBusRetryTick = now + 500;
                if (_bus.TryOpen(_cameraIndex))
                    continue;
            }
        }
        else
        {
            framebus::HotHeader fmt{};
            if (_bus.ReadFormat(fmt))
            {
                const uint32_t busWidth = fmt.width;
                const uint32_t busHeight = fmt.height;
                const uint32_t busFrameBytes = framebus::Yuy2Bytes(busWidth, busHeight);
                const LONG64 lastFrameId = _lastFrameId.load(std::memory_order_relaxed);

                if (isMjpeg)
                {
                    // Passthrough: copy the JFIF sidecar verbatim (no decode).
                    // Valid only at matching resolution — a JPEG can't be
                    // rescaled without decoding; dst=nullptr skips the YUY2 copy.
                    if (busWidth == width && busHeight == height && _jpegStaging)
                    {
                        uint32_t jpegLen = 0;
                        const LONG64 id = _bus.TryReadNewer(
                            nullptr, busFrameBytes,
                            _jpegStaging.get(), framebus::kMaxJpegBytes, jpegLen,
                            lastFrameId);
                        if (id != 0 && jpegLen != 0)
                        {
                            _lastFrameId.store(id, std::memory_order_relaxed);
                            _lastJpegLen = jpegLen;
                            break;
                        }
                    }
                }
                else if (busWidth == width && busHeight == height)
                {
                    const LONG64 id = _bus.TryReadNewer(staging.get(), busFrameBytes, lastFrameId);
                    if (id != 0)
                    {
                        _lastFrameId.store(id, std::memory_order_relaxed);
                        break;
                    }
                }
                else if (busFrameBytes > _busStagingBytes)
                {
                    // The bus outgrew this stream's scratch. Reachable when the
                    // host re-advertises a bigger geometry without re-registering
                    // (the split-pair wake path). Reading would overrun.
                    if (!_warnedSizeMismatch)
                    {
                        _warnedSizeMismatch = true;
                        VCamTrace(L"stream: BLACK -- bus grew to %ux%u (%u bytes) but this "
                                  L"stream's scratch is %u bytes; re-register to resize",
                                  busWidth, busHeight, busFrameBytes, _busStagingBytes);
                    }
                    FillYuy2Black(staging.get(), width, height);
                    break;
                }
                else
                {
                    const LONG64 id = _bus.TryReadNewer(_busStaging.get(), busFrameBytes, lastFrameId);
                    if (id != 0)
                    {
                        _lastFrameId.store(id, std::memory_order_relaxed);
                        switch (yuv::ScalePlanFor(busWidth, busHeight, width, height))
                        {
                        case yuv::ScalePlan::Upscale2x:
                            yuv::UpscaleYuy2_2x(_busStaging.get(), staging.get(), busWidth, busHeight);
                            break;
                        case yuv::ScalePlan::Downscale2x:
                            yuv::DownscaleYuy2_2x(_busStaging.get(), staging.get(), busWidth, busHeight);
                            break;
                        case yuv::ScalePlan::Direct:
                            // Unreachable: equal sizes are handled above.
                            memcpy(staging.get(), _busStaging.get(), busFrameBytes);
                            break;
                        case yuv::ScalePlan::Unsupported:
                        default:
                            // Reachable only if the client negotiated against a
                            // STALE type list -- the host changed resolution
                            // without re-registering the camera -- so every frame
                            // from here is black. Say so once.
                            if (!_warnedSizeMismatch)
                            {
                                _warnedSizeMismatch = true;
                                VCamTrace(L"stream: BLACK -- negotiated %ux%u but bus is %ux%u "
                                          L"(stale media types; host must re-register on a "
                                          L"resolution change)", width, height, busWidth, busHeight);
                            }
                            FillYuy2Black(staging.get(), width, height);
                            break;
                        }
                        break;
                    }
                }
            }
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
            break;
        const ULONGLONG remaining = deadline - now;
        const DWORD waitMs = remaining > 100 ? 100 : static_cast<DWORD>(remaining);
        if (_bus.IsOpen())
            _bus.WaitFrame(waitMs);          // woken by the writer per publish
        else
            Sleep(waitMs > 20 ? 20 : waitMs);  // host absent: cheap idle wait
    }

    // MJPEG samples are variable-size (the JFIF length); YUY2/NV12 are fixed.
    const DWORD outBytes = isMjpeg ? _lastJpegLen : frameBytes;
    if (isMjpeg && outBytes == 0)
    {
        // No JFIF available yet (cold start before the first frame, or host
        // absent). Skip this delivery rather than emit a malformed JPEG; the
        // client will request another sample.
        return S_OK;
    }

    ComPtr<IMFSample> sample;
    ComPtr<IMFMediaBuffer> buffer;
    HRESULT hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer(outBytes, &buffer);
    if (SUCCEEDED(hr))
    {
        BYTE* data = nullptr;
        DWORD maxLen = 0;
        hr = buffer->Lock(&data, &maxLen, nullptr);
        if (SUCCEEDED(hr))
        {
            if (isMjpeg)
            {
                // Passthrough: hand the source JFIF to the client untouched.
                memcpy(data, _jpegStaging.get(), outBytes);
            }
            else if (subtype == MFVideoFormat_YUY2)
            {
                // Native path: the bus frame is already YUY2 — no conversion.
                memcpy(data, staging.get(), outBytes);
            }
            else
            {
                yuv::Yuy2ToNv12(staging.get(), data, width, height);
            }
            buffer->Unlock();
            buffer->SetCurrentLength(outBytes);
        }
    }
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buffer.Get());
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(MFGetSystemTime());
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration(frameDuration);
    if (SUCCEEDED(hr) && token)
        hr = sample->SetUnknown(MFSampleExtension_Token, token);

    if (SUCCEEDED(hr))
    {
        AutoLock lock(_lock);
        if (_shutdown)
            return MF_E_SHUTDOWN;
        if (_state != MF_STREAM_STATE_RUNNING)
            return MF_E_MEDIA_SOURCE_WRONGSTATE;
        hr = _queue->QueueEventParamUnk(MEMediaSample, GUID_NULL, S_OK, sample.Get());
    }
    return hr;
}

// ----- IMFMediaStream2 --------------------------------------------------------

STDMETHODIMP MediaStream::SetStreamState(MF_STREAM_STATE value)
{
    switch (value)
    {
    case MF_STREAM_STATE_RUNNING:
    {
        PROPVARIANT empty;
        PropVariantInit(&empty);
        return Start(&empty);
    }
    case MF_STREAM_STATE_STOPPED:
        return Stop();
    default:
        return MF_E_INVALID_STATE_TRANSITION;
    }
}

STDMETHODIMP MediaStream::GetStreamState(MF_STREAM_STATE* value)
{
    if (!value)
        return E_POINTER;
    AutoLock lock(_lock);
    if (_shutdown)
        return MF_E_SHUTDOWN;
    *value = _state;
    return S_OK;
}

// ----- IKsControl --------------------------------------------------------------

STDMETHODIMP MediaStream::KsProperty(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsMethod(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaStream::KsEvent(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}

// ===========================================================================
// MediaSource
// ===========================================================================

MediaSource::MediaSource(int cameraIndex) : _cameraIndex(cameraIndex)
{
    DllAddRef();
}

HRESULT MediaSource::Initialize(IMFAttributes* activationAttributes)
{
    HRESULT hr = MFCreateEventQueue(&_queue);
    if (FAILED(hr)) return hr;

    hr = MFCreateAttributes(&_attributes, 2);
    if (FAILED(hr)) return hr;
    if (activationAttributes)
        activationAttributes->CopyAllItems(_attributes.Get());

    _stream.Attach(new (std::nothrow) MediaStream(this, _cameraIndex));
    if (!_stream)
        return E_OUTOFMEMORY;
    hr = _stream->Initialize();
    if (FAILED(hr)) return hr;

    IMFStreamDescriptor* descriptors[] = { _stream->Descriptor() };
    hr = MFCreatePresentationDescriptor(1, descriptors, &_pd);
    if (FAILED(hr)) return hr;
    hr = _pd->SelectStream(0);
    if (FAILED(hr)) return hr;

    VCamTrace(L"source: initialized");
    return S_OK;
}

// ----- IUnknown -------------------------------------------------------------

STDMETHODIMP MediaSource::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (riid == IID_IUnknown || riid == __uuidof(IMFMediaSourceEx) ||
        riid == __uuidof(IMFMediaSource) || riid == __uuidof(IMFMediaEventGenerator))
        *ppv = static_cast<IMFMediaSourceEx*>(this);
    else if (riid == __uuidof(IMFGetService))
        *ppv = static_cast<IMFGetService*>(this);
    else if (riid == __uuidof(IKsControl))
        *ppv = static_cast<IKsControl*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}

STDMETHODIMP_(ULONG) MediaSource::AddRef()
{
    return InterlockedIncrement(&_refCount);
}

STDMETHODIMP_(ULONG) MediaSource::Release()
{
    const ULONG ref = InterlockedDecrement(&_refCount);
    if (ref == 0)
    {
        delete this;
        DllRelease();
    }
    return ref;
}

// ----- IMFMediaEventGenerator ------------------------------------------------

STDMETHODIMP MediaSource::BeginGetEvent(IMFAsyncCallback* pCallback, IUnknown* punkState)
{
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return _queue->BeginGetEvent(pCallback, punkState);
}

STDMETHODIMP MediaSource::EndGetEvent(IMFAsyncResult* pResult, IMFMediaEvent** ppEvent)
{
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return _queue->EndGetEvent(pResult, ppEvent);
}

STDMETHODIMP MediaSource::GetEvent(DWORD dwFlags, IMFMediaEvent** ppEvent)
{
    ComPtr<IMFMediaEventQueue> queue;
    {
        AutoLock lock(_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        queue = _queue;
    }
    return queue->GetEvent(dwFlags, ppEvent);
}

STDMETHODIMP MediaSource::QueueEvent(MediaEventType met, REFGUID guidExtendedType,
                                     HRESULT hrStatus, const PROPVARIANT* pvValue)
{
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return _queue->QueueEventParamVar(met, guidExtendedType, hrStatus, pvValue);
}

// ----- IMFMediaSource ---------------------------------------------------------

STDMETHODIMP MediaSource::CreatePresentationDescriptor(IMFPresentationDescriptor** ppPD)
{
    if (!ppPD)
        return E_POINTER;
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    return _pd->Clone(ppPD);
}

STDMETHODIMP MediaSource::GetCharacteristics(DWORD* pdwCharacteristics)
{
    if (!pdwCharacteristics)
        return E_POINTER;
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    *pdwCharacteristics = MFMEDIASOURCE_IS_LIVE;
    return S_OK;
}

STDMETHODIMP MediaSource::Pause()
{
    return MF_E_INVALID_STATE_TRANSITION;
}

STDMETHODIMP MediaSource::Start(IMFPresentationDescriptor* pPD, const GUID* pguidTimeFormat,
                                const PROPVARIANT* pvarStartPosition)
{
    if (pguidTimeFormat && *pguidTimeFormat != GUID_NULL)
        return MF_E_UNSUPPORTED_TIME_FORMAT;

    ComPtr<MediaStream> stream;
    bool firstStart = false;
    {
        AutoLock lock(_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        if (!pPD)
            return E_INVALIDARG;

        DWORD count = 0;
        hr = pPD->GetStreamDescriptorCount(&count);
        if (FAILED(hr) || count != 1)
            return MF_E_UNSUPPORTED_REPRESENTATION;

        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        hr = pPD->GetStreamDescriptorByIndex(0, &selected, &sd);
        if (FAILED(hr) || !selected)
            return MF_E_UNSUPPORTED_REPRESENTATION;

        firstStart = !_streamNotified;
        _streamNotified = true;
        _state = State::Started;
        stream = _stream;

        hr = _queue->QueueEventParamUnk(firstStart ? MENewStream : MEUpdatedStream,
                                        GUID_NULL, S_OK,
                                        static_cast<IMFMediaStream2*>(stream.Get()));
        if (FAILED(hr)) return hr;
    }

    HRESULT hr = stream->Start(pvarStartPosition);
    if (FAILED(hr)) return hr;

    {
        AutoLock lock(_lock);
        hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        hr = _queue->QueueEventParamVar(MESourceStarted, GUID_NULL, S_OK, pvarStartPosition);
    }
    VCamTrace(L"source: started (first=%d)", firstStart ? 1 : 0);
    return hr;
}

STDMETHODIMP MediaSource::Stop()
{
    ComPtr<MediaStream> stream;
    {
        AutoLock lock(_lock);
        HRESULT hr = CheckShutdown();
        if (FAILED(hr)) return hr;
        _state = State::Stopped;
        stream = _stream;
    }

    if (stream)
        stream->Stop();

    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    VCamTrace(L"source: stopped");
    return _queue->QueueEventParamVar(MESourceStopped, GUID_NULL, S_OK, nullptr);
}

STDMETHODIMP MediaSource::Shutdown()
{
    AutoLock lock(_lock);
    if (_state == State::Shutdown)
        return MF_E_SHUTDOWN;
    _state = State::Shutdown;

    if (_stream)
        _stream->Shutdown();
    if (_queue)
        _queue->Shutdown();

    _stream.Reset();
    _pd.Reset();
    _attributes.Reset();
    VCamTrace(L"source: shutdown");
    return S_OK;
}

// ----- IMFMediaSourceEx --------------------------------------------------------

STDMETHODIMP MediaSource::GetSourceAttributes(IMFAttributes** ppAttributes)
{
    if (!ppAttributes)
        return E_POINTER;
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    *ppAttributes = _attributes.Get();
    (*ppAttributes)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaSource::GetStreamAttributes(DWORD dwStreamIdentifier, IMFAttributes** ppAttributes)
{
    if (!ppAttributes)
        return E_POINTER;
    AutoLock lock(_lock);
    HRESULT hr = CheckShutdown();
    if (FAILED(hr)) return hr;
    if (dwStreamIdentifier != 0 || !_stream)
        return MF_E_INVALIDSTREAMNUMBER;
    *ppAttributes = _stream->Attributes();
    (*ppAttributes)->AddRef();
    return S_OK;
}

STDMETHODIMP MediaSource::SetD3DManager(IUnknown*)
{
    // Samples are system-memory; the Frame Server handles any GPU upload.
    return S_OK;
}

// ----- IMFGetService -------------------------------------------------------------

STDMETHODIMP MediaSource::GetService(REFGUID, REFIID, LPVOID* ppvObject)
{
    if (ppvObject)
        *ppvObject = nullptr;
    return MF_E_UNSUPPORTED_SERVICE;
}

// ----- IKsControl ----------------------------------------------------------------

STDMETHODIMP MediaSource::KsProperty(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsMethod(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
STDMETHODIMP MediaSource::KsEvent(void*, ULONG, void*, ULONG, ULONG* BytesReturned)
{
    if (BytesReturned) *BytesReturned = 0;
    return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);
}
