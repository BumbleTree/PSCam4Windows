#include <windows.h>
#include <objbase.h>
#include <sddl.h>
#include <mfapi.h>
#include <mfidl.h>
#include <algorithm>
#include <atomic>
#include <thread>
#include "TestSupport.h"

static std::wstring TestName(LPCWSTR name)
{ return L"Local\\PSCam4Win.SourceTests." + std::to_wstring(GetCurrentProcessId()) + L"." + (name + 7); }
static HANDLE TestCreateMap(HANDLE file, LPSECURITY_ATTRIBUTES sa, DWORD protect, DWORD hi, DWORD lo, LPCWSTR name)
{ return CreateFileMappingW(file, sa, protect, hi, lo, TestName(name).c_str()); }
static HANDLE TestOpenMap(DWORD access, BOOL inherit, LPCWSTR name)
{ return OpenFileMappingW(access, inherit, TestName(name).c_str()); }
static HANDLE TestCreateEvent(LPSECURITY_ATTRIBUTES sa, BOOL manual, BOOL initial, LPCWSTR name)
{ return CreateEventW(sa, manual, initial, TestName(name).c_str()); }
static HANDLE TestOpenEvent(DWORD access, BOOL inherit, LPCWSTR name)
{ return OpenEventW(access, inherit, TestName(name).c_str()); }
#define CreateFileMappingW TestCreateMap
#define OpenFileMappingW TestOpenMap
#define CreateEventW TestCreateEvent
#define OpenEventW TestOpenEvent
#include "../source/VCamSource.h"
#undef CreateFileMappingW
#undef OpenFileMappingW
#undef CreateEventW
#undef OpenEventW

static bool failPresentation = false;
static HRESULT TestCreatePD(DWORD count, IMFStreamDescriptor** streams, IMFPresentationDescriptor** out)
{
    if (failPresentation) { *out = nullptr; return E_OUTOFMEMORY; }
    return MFCreatePresentationDescriptor(count, streams, out);
}
#define MFCreatePresentationDescriptor TestCreatePD
static HANDLE sampleEntered = nullptr, sampleContinue = nullptr;
static HRESULT TestCreateSample(IMFSample** out)
{
    if (sampleEntered) { SetEvent(sampleEntered); WaitForSingleObject(sampleContinue, INFINITE); }
    return MFCreateSample(out);
}
#define MFCreateSample TestCreateSample
#include "../source/VCamSource.cpp"
#undef MFCreatePresentationDescriptor
#undef MFCreateSample
#include "../source/dllmain.cpp"
#define objects g_objectCount

static ComPtr<IMFSample> NextSample(MediaStream* stream)
{
    ComPtr<IMFMediaEvent> event;
    while (SUCCEEDED(stream->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event)))
    {
        MediaEventType type{}; event->GetType(&type);
        if (type == MEMediaSample)
        {
            PROPVARIANT value; PropVariantInit(&value);
            ComPtr<IMFSample> sample;
            if (SUCCEEDED(event->GetValue(&value)) && value.vt == VT_UNKNOWN)
                value.punkVal->QueryInterface(IID_PPV_ARGS(&sample));
            PropVariantClear(&value);
            return sample;
        }
        event.Reset();
    }
    return {};
}

static bool IsBlackJpeg(IMFSample* sample, int width, int height)
{
    if (!sample) return false;
    ComPtr<IMFMediaBuffer> buffer;
    if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
    BYTE* data = nullptr; DWORD length = 0;
    if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
    tjhandle decoder = tjInitDecompress();
    int w = 0, h = 0, subsampling = 0, color = 0;
    std::vector<BYTE> rgb(static_cast<size_t>(width) * height * 3);
    const bool ok = decoder && tjDecompressHeader3(decoder, data, length, &w, &h, &subsampling, &color) == 0 &&
        w == width && h == height && tjDecompress2(decoder, data, length, rgb.data(), width, 0, height, TJPF_RGB, 0) == 0 &&
        std::all_of(rgb.begin(), rgb.end(), [](BYTE b) { return b < 3; });
    if (decoder) tjDestroy(decoder);
    buffer->Unlock(); return ok;
}

static void TestMjpeg(framebus::Writer& writer)
{
    const framebus::ColdMode mode{320,240,30,framebus::kFmtYUY2 | framebus::kFmtMJPEG};
    writer.WriteColdBlock(1, framebus::kFmtYUY2, &mode, 1); writer.PublishBlack();
    auto* parent = new MediaSource(0);
    auto* stream = new MediaStream(parent, 0);
    const HRESULT initialized = stream->Initialize();
    Check(SUCCEEDED(initialized), "MJPEG source initializes from isolated capability table");
    if (FAILED(initialized)) { stream->Shutdown(); stream->Release(); parent->Release(); return; }
    ComPtr<IMFMediaTypeHandler> handler; stream->Descriptor()->GetMediaTypeHandler(&handler);
    ComPtr<IMFMediaType> type;
    DWORD count = 0; handler->GetMediaTypeCount(&count);
    for (DWORD i = 0; i < count; ++i)
    {
        GUID sub{}; handler->GetMediaTypeByIndex(i, &type); type->GetGUID(MF_MT_SUBTYPE, &sub);
        if (sub == MFVideoFormat_MJPG) break;
        type.Reset();
    }
    Check(type && SUCCEEDED(handler->SetCurrentMediaType(type.Get())), "MJPEG negotiation selects compressed media type");
    PROPVARIANT position; PropVariantInit(&position);
    Check(SUCCEEDED(stream->Start(&position)) && SUCCEEDED(stream->RequestSample(nullptr)), "cold MJPEG request succeeds before camera JPEG arrival");
    auto first = NextSample(stream);
    Check(IsBlackJpeg(first.Get(), 320, 240), "cold-start request queues a valid black JPEG at negotiated dimensions");
    UINT32 clean = 0;
    Check(first && SUCCEEDED(first->GetUINT32(MFSampleExtension_CleanPoint, &clean)) && clean, "MJPEG fallback is an independent clean-point sample");
    LONGLONG timestamp = 0;
    if (first) first->GetSampleTime(&timestamp);
    first.Reset();

    sampleEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    sampleContinue = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT oldResult = E_FAIL, newResult = E_FAIL;
    std::thread request([&] { oldResult = stream->RequestSample(nullptr); });
    Check(WaitForSingleObject(sampleEntered, 2000) == WAIT_OBJECT_0, "old sample delivery paused before publication");
    stream->Stop();
    std::atomic<bool> restarted{false};
    std::thread start([&] { newResult = stream->Start(&position); restarted.store(true); });
    Sleep(30);
    Check(!restarted.load(), "restart cannot replace JPEG cache during an old delivery");
    SetEvent(sampleContinue); request.join(); start.join();
    CloseHandle(sampleEntered); CloseHandle(sampleContinue); sampleEntered = sampleContinue = nullptr;
    Check(oldResult == MF_E_MEDIA_SOURCE_WRONGSTATE && SUCCEEDED(newResult) && !NextSample(stream),
          "Stop/Start discards the obsolete delivery before the new generation starts");
    Check(SUCCEEDED(stream->RequestSample(nullptr)), "new generation accepts sample requests");
    auto next = NextSample(stream); LONGLONG nextTimestamp = 0;
    if (next) next->GetSampleTime(&nextTimestamp);
    Check(IsBlackJpeg(next.Get(), 320, 240) && nextTimestamp > timestamp,
          "restart regenerates valid fallback and timestamps remain increasing");
    next.Reset();
    stream->Stop();
    MFSetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, 640, 480);
    Check(SUCCEEDED(stream->Start(&position)) && SUCCEEDED(stream->RequestSample(nullptr)),
          "MJPEG renegotiation at another valid size succeeds");
    next = NextSample(stream);
    Check(IsBlackJpeg(next.Get(), 640, 480), "JPEG fallback cache is rebuilt at new negotiated geometry");
    next.Reset(); stream->Stop();
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 3000000000u, 1500000000u);
    Check(stream->Start(&position) == MF_E_INVALIDMEDIATYPE, "oversized rational fields retain the negotiated-format validation limits");
    MFSetAttributeRatio(type.Get(), MF_MT_FRAME_RATE, 60000, 1001);
    Check(SUCCEEDED(stream->Start(&position)), "bounded fractional frame rates negotiate successfully");
    writer.UpdateFormat(640, 480, 60000, 1001);
    std::vector<BYTE> black;
    jpeg::Black(640, 480, black);
    std::vector<BYTE> pixels(640 * 480 * 2, 0);
    writer.PublishWithJpeg(pixels.data(), static_cast<uint32_t>(pixels.size()), black.data(), static_cast<uint32_t>(black.size()));
    stream->RequestSample(nullptr); next = NextSample(stream);
    LONGLONG duration = 0;
    Check(next && SUCCEEDED(next->GetSampleDuration(&duration)) && duration == 166833,
          "fractional frame rate produces the correct positive sample duration");
    next.Reset(); type.Reset(); handler.Reset();
    stream->Shutdown(); stream->Release(); parent->Release();
}

static void TestActivator()
{
    ComPtr<IMFActivate> activation;
    Check(SUCCEEDED(Activator::CreateInstance(IID_PPV_ARGS(&activation), 0)), "real COM activator creates for an isolated camera slot");
    if (!activation) return;
    activation->SetUINT32(GUID_NULL, 42);
    ComPtr<IMFMediaSource> first, second;
    Check(SUCCEEDED(activation->ActivateObject(IID_PPV_ARGS(&first))) &&
          SUCCEEDED(activation->ActivateObject(IID_PPV_ARGS(&second))) && first.Get() != second.Get(),
          "activations create independently owned sources instead of caching a singleton");
    Check(activation->DetachObject() == S_OK,
          "DetachObject releases the activator's reference without shutting the source down");
    {
        ComPtr<IMFPresentationDescriptor> live;
        Check(second && SUCCEEDED(second->CreatePresentationDescriptor(&live)),
              "a detached source keeps working for its owner");
    }
    ComPtr<IMFMediaSource> third;
    Check(SUCCEEDED(activation->ActivateObject(IID_PPV_ARGS(&third))) && third &&
          activation->ShutdownObject() == S_OK,
          "ShutdownObject shuts down the latest activation");
    {
        ComPtr<IMFPresentationDescriptor> dead;
        Check(third && third->CreatePresentationDescriptor(&dead) == MF_E_SHUTDOWN,
              "the shut-down source reports MF_E_SHUTDOWN");
        ComPtr<IMFPresentationDescriptor> live;
        Check(first && SUCCEEDED(first->CreatePresentationDescriptor(&live)),
              "ShutdownObject leaves earlier, independently owned sources running");
    }
    third.Reset();
    if (first)
    {
        ComPtr<IMFMediaSourceEx> extended; first.As(&extended);
        ComPtr<IMFAttributes> attrs; extended->GetSourceAttributes(&attrs);
        UINT32 value = 0;
        Check(SUCCEEDED(attrs->GetUINT32(GUID_NULL, &value)) && value == 42,
              "activation attributes reach each independent source");
    }
    activation.Reset();
    ComPtr<IMFPresentationDescriptor> pd;
    Check(first && SUCCEEDED(first->CreatePresentationDescriptor(&pd)), "source remains valid after activator release");
    pd.Reset();
    if (first) first->Shutdown();
    if (second) second->Shutdown();
    first.Reset(); second.Reset();
    Check(objects == 0, "independent activated sources release every module object on coordinated shutdown");
    ComPtr<IMFActivate> failed;
    Activator::CreateInstance(IID_PPV_ARGS(&failed), 0);
    failPresentation = true;
    ComPtr<IMFMediaSource> rejected;
    Check(failed->ActivateObject(IID_PPV_ARGS(&rejected)) == E_OUTOFMEMORY && !rejected && objects == 1,
          "failed activation unwinds its source/stream cycle while retaining only the activator");
    failPresentation = false;
    Check(failed->ActivateObject(IID_IMFMediaSink, reinterpret_cast<void**>(rejected.GetAddressOf())) == E_NOINTERFACE &&
          !rejected && objects == 1, "unsupported activation interface shuts down the otherwise initialized source");
    failed.Reset();
    Check(objects == 0, "failed activator tests leave no module objects");
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Check(SUCCEEDED(com) && SUCCEEDED(MFStartup(MF_VERSION)), "Media Foundation starts for isolated source tests");
    if (testFailures) return 1;
    framebus::Writer writer;
    Check(writer.Create(0, 320, 240, 30, 1), "isolated source bus created");
    const framebus::ColdMode mode{ 320, 240, 30, framebus::kFmtYUY2 };
    writer.WriteColdBlock(1, framebus::kFmtYUY2, &mode, 1); writer.PublishBlack();
    failPresentation = true;
    auto* failed = new MediaSource(0);
    Check(failed->Initialize(nullptr) == E_OUTOFMEMORY, "fault injected after successful stream initialization");
    Check(failed->Release() == 0 && objects == 0, "failed initialization releases the source/stream cycle");
    failPresentation = false;
    auto* formatParent = new MediaSource(0);
    auto* formatStream = new MediaStream(formatParent, 0);
    Check(SUCCEEDED(formatStream->Initialize()), "format-validation stream initializes");
    ComPtr<IMFMediaTypeHandler> handler;
    ComPtr<IMFMediaType> negotiated;
    formatStream->Descriptor()->GetMediaTypeHandler(&handler);
    handler->GetCurrentMediaType(&negotiated);
    PROPVARIANT start; PropVariantInit(&start);
    MFSetAttributeSize(negotiated.Get(), MF_MT_FRAME_SIZE, 3, 3);
    Check(formatStream->Start(&start) == MF_E_INVALIDMEDIATYPE,
          "odd negotiated geometry is rejected before allocating staging");
    MFSetAttributeSize(negotiated.Get(), MF_MT_FRAME_SIZE, 320, 240);
    MFSetAttributeRatio(negotiated.Get(), MF_MT_FRAME_RATE, 1000000000, 1);
    Check(formatStream->Start(&start) == MF_E_INVALIDMEDIATYPE, "unbounded negotiated rate rejected");
    MFSetAttributeRatio(negotiated.Get(), MF_MT_FRAME_RATE, 30, 1);
    negotiated->SetGUID(MF_MT_SUBTYPE, GUID_NULL);
    Check(formatStream->Start(&start) == MF_E_INVALIDMEDIATYPE, "unsupported negotiated subtype rejected");
    MF_STREAM_STATE state{};
    Check(SUCCEEDED(formatStream->GetStreamState(&state)) && state == MF_STREAM_STATE_STOPPED,
          "failed format validation preserves stopped stream state");
    negotiated.Reset(); handler.Reset();
    formatStream->Shutdown(); formatStream->Release(); formatParent->Release();
    auto* source = new MediaSource(0);
    Check(SUCCEEDED(source->Initialize(nullptr)), "ordinary source initialization succeeds");
    ComPtr<IMFPresentationDescriptor> pd;
    Check(SUCCEEDED(source->CreatePresentationDescriptor(&pd)), "presentation descriptor available");
    PROPVARIANT position; PropVariantInit(&position);
    Check(SUCCEEDED(source->Start(pd.Get(), nullptr, &position)), "ordinary source starts");
    ComPtr<IMFMediaEvent> event;
    ComPtr<IMFMediaStream> held;
    while (SUCCEEDED(source->GetEvent(MF_EVENT_FLAG_NO_WAIT, &event)))
    {
        MediaEventType type{}; event->GetType(&type);
        if (type == MENewStream)
        {
            PROPVARIANT value; PropVariantInit(&value);
            if (SUCCEEDED(event->GetValue(&value)) && value.vt == VT_UNKNOWN)
                value.punkVal->QueryInterface(IID_PPV_ARGS(&held));
            PropVariantClear(&value);
        }
        event.Reset();
    }
    Check(held != nullptr, "consumer retains an independent stream reference");
    Check(SUCCEEDED(source->Shutdown()) && source->Release() == 0,
          "source shutdown is safe while consumer retains stream");
    ComPtr<IMFMediaSource> parent;
    Check(held && held->GetMediaSource(&parent) == MF_E_SHUTDOWN,
          "retained stream rejects access to the shut-down parent");
    held.Reset(); pd.Reset();
    Check(objects == 0, "ordinary shutdown releases every module object");
    TestMjpeg(writer);
    Check(objects == 0, "MJPEG restart tests release all source and stream objects");
    TestActivator();
    MFShutdown(); CoUninitialize();
    printf("source: %d failures\n", testFailures);
    return testFailures ? 1 : 0;
}
