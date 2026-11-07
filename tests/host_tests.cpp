#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cmath>
#include "TestSupport.h"
#include "../common/AudioRate.h"
#include "../host/RecordingCommands.h"
#include "../host/WavWriter.h"
#include "../host/DeviceRegistry.h"
#include "../host/Ps4SlotAssign.h"
static std::wstring AudioName(LPCWSTR name)
{ return L"Local\\PSCam4Win.AudioTests." + std::to_wstring(GetCurrentProcessId()) + L"." + (name + 7); }
static HANDLE CreateAudioMap(HANDLE f, LPSECURITY_ATTRIBUTES sa, DWORD p, DWORD hi, DWORD lo, LPCWSTR name)
{ return CreateFileMappingW(f, sa, p, hi, lo, AudioName(name).c_str()); }
static HANDLE OpenAudioMap(DWORD access, BOOL inherit, LPCWSTR name)
{ return OpenFileMappingW(access, inherit, AudioName(name).c_str()); }
#define CreateFileMappingW CreateAudioMap
#define OpenFileMappingW OpenAudioMap
static ULONGLONG audioTick = 5000;
static ULONGLONG AudioTick() { return audioTick; }
#define GetTickCount64 AudioTick
#include "../common/AudioStatusBus.h"
#undef CreateFileMappingW
#undef OpenFileMappingW
#undef GetTickCount64
#define private public
#include "../transports/usb_ps4/Ps4CaptureSource.h"
#include "../transports/usb_ps4/Ps4ViewDevice.h"
#undef private
#include "../transports/usb_ps4/Ps4CaptureSource.cpp"

static uint32_t Word(const std::vector<BYTE>& bytes, size_t offset)
{ uint32_t v = 0; if (bytes.size() >= offset + 4) memcpy(&v, bytes.data() + offset, 4); return v; }

static void TestRates()
{
    for (uint32_t sink : { 44100u, 48000u, 96000u, 192000u })
    {
        const double nominal = 48000.0 / sink;
        Check(std::abs(audiorender::TargetRatio(48000, sink, 1920, 1920) - nominal) < 1e-9,
              "output rate retains the correct nominal source/sink ratio");
        const double fast = audiorender::TargetRatio(48000, sink, 24000, 1920);
        const double slow = audiorender::TargetRatio(48000, sink, 0, 1920);
        Check(fast <= nominal * 1.03 && slow >= nominal * 0.97,
              "drift correction is bounded relative to the nominal ratio");
        double phase = 0; uint64_t consumed = 0;
        for (uint32_t i = 0; i < sink; ++i)
        { phase += nominal; const auto n = uint64_t(phase); consumed += n; phase -= double(n); }
        Check(consumed >= 47999 && consumed <= 48001, "one output second consumes one source second at every supported rate");
    }
    Check(audiorender::SourceRate(0) == 48000 && audiorender::SourceRate(48023000) == 48023,
          "routing uses nominal source rate before measurements and retains measured drift");
}

static void TestRecording()
{
    RecordingCommands commands;
    commands.Start(L"first.wav"); const auto first = commands.Snapshot();
    commands.Stop(); commands.Start(L"second.wav"); const auto second = commands.Snapshot();
    Check(second.generation > first.generation && second.path == L"second.wav" && second.start,
          "rapid Stop/Start publishes a new immutable recording generation");
    commands.Acknowledge(first.generation, RecordingCommands::State::Failed);
    Check(commands.Status() == RecordingCommands::State::Starting && commands.Snapshot().start,
          "an obsolete failure cannot cancel a newer recording request");
    commands.Acknowledge(second.generation, RecordingCommands::State::Recording);
    Check(commands.Status() == RecordingCommands::State::Recording, "capture acknowledges successful recording startup");
    commands.Acknowledge(second.generation, RecordingCommands::State::Failed);
    Check(!commands.Snapshot().start && commands.Status() == RecordingCommands::State::Failed,
          "current recording failure is visible and stops the request");

    const std::wstring dir = ScratchDirectory();
    Check(!dir.empty(), "recording scratch directory created");
    if (dir.empty()) return;
    const std::wstring file = dir + L"\\audio.wav";
    std::vector<int16_t> pcm(160, 123);
    WavWriter wav(64);
    Check(wav.Open(file.c_str(), 48000, 4), "exclusive WAV opens with a checked header");
    wav.Write(pcm.data(), 20); wav.Close();
    Check(!wav.WriteFailed(), "recording segments finalize successfully");
    const auto a = ReadBytes(file), b = ReadBytes(dir + L"\\audio-part2.wav"), c = ReadBytes(dir + L"\\audio-part3.wav");
    Check(a.size() == 108 && b.size() == 108 && c.size() == 76 &&
          Word(a, 4) == 100 && Word(a, 40) == 64 && Word(c, 40) == 32,
          "rollover preserves all PCM frames and writes valid bounded RIFF lengths");
    Check(!wav.Open(file.c_str(), 48000, 4) && ReadBytes(file) == a,
          "a name collision cannot truncate an existing recording");
    WavWriter blocked(64);
    Check(blocked.Open((dir + L"\\blocked.wav").c_str(), 48000, 4), "rollover failure fixture opens");
    WriteBytes(dir + L"\\blocked-part2.wav", { 1, 2, 3 });
    blocked.Write(pcm.data(), 20); blocked.Close();
    Check(blocked.WriteFailed() && ReadBytes(dir + L"\\blocked-part2.wav") == std::vector<BYTE>({1, 2, 3}),
          "segment collision reports failure and preserves the existing segment");
    const std::wstring dotted = dir + L"\\folder.with.dot";
    CreateDirectoryW(dotted.c_str(), nullptr);
    WavWriter extensionless(16);
    Check(extensionless.Open((dotted + L"\\recording").c_str(), 48000, 4), "extensionless recording opens inside a dotted directory");
    extensionless.Write(pcm.data(), 3); extensionless.Close();
    Check(!extensionless.WriteFailed() && ReadBytes(dotted + L"\\recording-part2").size() == 52,
          "segment suffix applies to the filename rather than a dotted parent directory");
    DeleteFileW((dotted + L"\\recording").c_str()); DeleteFileW((dotted + L"\\recording-part2").c_str());
    RemoveDirectoryW(dotted.c_str());
    for (const auto* name : { L"audio.wav", L"audio-part2.wav", L"audio-part3.wav", L"blocked.wav", L"blocked-part2.wav" })
        DeleteFileW((dir + L"\\" + name).c_str());
    RemoveDirectoryW(dir.c_str());
}

static void TestPs4()
{
    const auto leftMode = deviceregistry::ReframePs4Mode(VideoMode{2560,800,30}, Ps4ViewKind::SideBySide, Ps4ViewKind::Left);
    const auto sbsMode = deviceregistry::ReframePs4Mode(leftMode, Ps4ViewKind::Left, Ps4ViewKind::SideBySide);
    Check(leftMode.width == 1280 && leftMode.height == 800 && leftMode.fps == 30 && sbsMode.width == 2560 && sbsMode.fps == 30,
          "split/view transitions preserve physical eye geometry and the selected frame rate");
    DeviceProfile profile{}; profile.controlMask = CTRL_MICARRAY;
    deviceregistry::Ps4SlotInfo info; info.isPs4 = true; info.split = true; info.view = Ps4ViewKind::Right;
    Check(!deviceregistry::OwnsMicrophone(&profile, info), "split Right cannot take a microphone wake hold");
    info.split = false;
    Check(deviceregistry::OwnsMicrophone(&profile, info), "a standalone Right view owns its physical microphone");
    info.split = true; info.view = Ps4ViewKind::Left;
    Check(deviceregistry::OwnsMicrophone(&profile, info), "split Left owns microphone wake and routing");
    info.portPath = "camera-one";
    auto changed = info; changed.portPath = "camera-two";
    Check(!deviceregistry::SamePs4DeviceRole(info, changed), "reassigning a slot to another physical PS4 requires a new device");
    changed = info; changed.view = Ps4ViewKind::Right;
    Check(!deviceregistry::SamePs4DeviceRole(info, changed), "changing split hardware ownership requires a new device adapter");

    ps4mic::MicDsp dsp;
    int16_t raw[4][8]{}; int16_t output[ps4mic::MicDsp::kMaxEmit * 4 + 8]{};
    for (const auto& input : { std::pair<uint32_t,uint32_t>{0,0}, {UINT32_MAX,UINT32_MAX}, {1,1} })
    {
        dsp.Reset(input.first, input.second);
        bool bounded = true;
        for (int i = 0; i < 1024; ++i)
        {
            std::fill(std::begin(output), std::end(output), int16_t(12345));
            const auto n = dsp.PushRow(raw, output);
            bounded &= n <= ps4mic::MicDsp::kMaxEmit && std::all_of(output + ps4mic::MicDsp::kMaxEmit * 4,
                      std::end(output), [](int16_t x) { return x == 12345; });
        }
        Check(bounded, "invalid DSP geometry cannot construct a zero/overflowing step or overrun row output");
    }

    auto source = std::shared_ptr<ps4::Ps4CaptureSource>(new ps4::Ps4CaptureSource("isolated-test"));
    source->_geom = *ps4::FindIspGeometry(320, 192, 60);
    const auto geom = source->_geom;
    std::vector<uint8_t> combined(geom.FrameBytes(), 0);
    for (uint32_t y = 0; y < geom.eyeH; ++y)
        for (uint32_t x = 0; x < geom.BandBytes(); ++x)
        {
            combined[size_t(y) * geom.RowBytes() + geom.LeftOff() + x] = uint8_t(x + y);
            combined[size_t(y) * geom.RowBytes() + geom.RightOff() + x] = uint8_t(x + y + 50);
        }
    source->PublishIspFrame(combined.data(), 0, false);
    // Change committed configuration without publishing: the retained pixels
    // must still report their own geometry, not the new desired configuration.
    source->_geom = *ps4::FindIspGeometry(640, 400, 60);
    for (bool flipH : { false, true }) for (bool flipV : { false, true })
    {
        Ps4ViewDevice view("isolated-test", Ps4ViewKind::SideBySide);
        view._source = source; view._useIsp = true; view._flipH = flipH; view._flipV = flipV;
        view._bayerL.resize(ps4::kEyeBayerBytes); view._bayerR.resize(ps4::kEyeBayerBytes);
        view._yuy2Out.reset(new uint8_t[ps4::kEyeBayerBytes * 2]);
        AcquiredFrame frame{};
        const bool got = view.AcquireFrame(frame, 0, false);
        Check(got && frame.mode.width == 640 && frame.mode.height == 192 && frame.yuy2Bytes == 640 * 192 * 2,
              "PS4 pixels and delivered mode come from one latched snapshot");
        bool matches = got;
        for (uint32_t y = 0; y < geom.eyeH && matches; ++y)
            for (uint32_t x = 0; x < geom.eyeW / 2 && matches; ++x)
                for (int eye = 0; eye < 2; ++eye)
                {
                    const auto* src = combined.data() + size_t(flipV ? geom.eyeH - 1 - y : y) * geom.RowBytes() +
                        (eye ? geom.RightOff() : geom.LeftOff()) + size_t(flipH ? geom.eyeW / 2 - 1 - x : x) * 4;
                    const auto* dst = frame.yuy2 + size_t(y) * geom.BandBytes() * 2 + eye * geom.BandBytes() + x * 4;
                    matches &= dst[0] == src[flipH ? 2 : 0] && dst[1] == src[1] && dst[2] == src[flipH ? 0 : 2] && dst[3] == src[3];
                }
        Check(matches && view._eyeYuyvL.empty() && view._eyeYuyvR.empty(),
              "direct SBS composition preserves every pixel and flip without eye scratch buffers");
    }
    source->StopIso();
    Check(source->SnapshotPlanes(nullptr, nullptr) == 0 && source->WaitNewer(0, 1) == 0,
          "stopped PS4 session cannot expose the previous image as a new frame");
    source->_xfersInFlight.store(1);
    std::atomic<bool> drained{false};
    std::thread stop([&] { source->StopIso(); drained.store(true); });
    Sleep(30);
    Check(!drained.load(), "PS4 teardown retains state while a cancellation callback is delayed");
    source->_xfersInFlight.store(0);
    stop.join();
    Check(drained.load(), "PS4 teardown completes after the terminal callback drains");
    source->_awakeUsers = 1;
    Check(source->RequestGeometry(1280, 800, 60, true) && source->Geometry().eyeW == 640,
          "a split secondary cannot change an active sibling's hardware mode");
    source->_awakeUsers = 0;
    Check(source->RequestGeometry(1280, 800, 60, true) && source->Geometry().eyeW == 1280,
          "a first-awake secondary can select its owner's mode on an idle source");
    libusb_transfer* lost = libusb_alloc_transfer(0);
    lost->status = LIBUSB_TRANSFER_NO_DEVICE;
    source->_xfersInFlight.store(1);
    source->HandleIso(lost);
    Check(source->_deviceLost.load() && source->_xfersInFlight.load() == 0,
          "terminal USB disconnect invalidates the cached PS4 session and drains the transfer");
    libusb_free_transfer(lost);
}

static void TestAudioStatus()
{
    audiostatus::Writer first, second;
    Check(first.Create(1) && second.Create(2), "isolated per-owner audio status mappings create");
    audiostatus::Block a{}, b{}, out{};
    a.slot = 1; a.channels = 4; a.levels[0] = 111;
    b.slot = 2; b.channels = 4; b.levels[0] = 222;
    first.Publish(a); second.Publish(b);
    audiostatus::Reader reader;
    Check(reader.Open(1) && reader.Read(out) && out.slot == 1 && out.levels[0] == 111,
          "the first microphone status is independent of a second writer");
    Check(reader.Open(2) && reader.Read(out) && out.slot == 2 && out.levels[0] == 222,
          "reader rebinds to the selected microphone instead of retaining another slot");
    Check(reader.OpenAny() && reader.Read(out) && out.slot == 1,
          "diagnostics find a fresh per-slot writer without the legacy global block");
    audioTick += audiostatus::kFreshMs + 1;
    Check(reader.OpenAny() && reader.Read(out) && !audiostatus::Reader::Fresh(out),
          "diagnostics retain a stale per-slot snapshot when every microphone sleeps");
    second.Publish(b);
    Check(reader.OpenAny() && reader.Read(out) && out.slot == 2 && audiostatus::Reader::Fresh(out),
          "a fresh microphone is preferred over another slot's stale telemetry");
    HANDLE token = nullptr, restricted = nullptr;
    BYTE sid[SECURITY_MAX_SID_SIZE]; DWORD bytes = sizeof(sid);
    CreateWellKnownSid(WinWorldSid, nullptr, sid, &bytes);
    SID_AND_ATTRIBUTES restricting{sid,0};
    const bool ready = OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY, &token) &&
        CreateRestrictedToken(token, DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr, 1, &restricting, &restricted) &&
        ImpersonateLoggedOnUser(restricted);
    Check(ready, "audio status read/write policy fixture impersonates a restricted consumer");
    if (ready)
    {
        HANDLE writable = OpenAudioMap(FILE_MAP_WRITE, FALSE, audiostatus::Name(1).c_str());
        HANDLE readable = OpenAudioMap(FILE_MAP_READ, FALSE, audiostatus::Name(1).c_str());
        RevertToSelf();
        Check(!writable && readable, "audio status consumers can read but cannot modify telemetry");
        if (writable) CloseHandle(writable);
        if (readable) CloseHandle(readable);
    }
    if (restricted) CloseHandle(restricted);
    if (token) CloseHandle(token);
}

static void MeasureSbs()
{
    auto source = std::shared_ptr<ps4::Ps4CaptureSource>(new ps4::Ps4CaptureSource("copy-benchmark"));
    const auto geom = source->Geometry();
    std::vector<uint8_t> combined(geom.FrameBytes(), 128);
    source->PublishIspFrame(combined.data(), 0, false);
    Ps4ViewDevice view("copy-benchmark", Ps4ViewKind::SideBySide);
    view._source = source; view._useIsp = true;
    view._bayerL.resize(ps4::kEyeBayerBytes); view._bayerR.resize(ps4::kEyeBayerBytes);
    view._yuy2Out.reset(new uint8_t[ps4::kEyeBayerBytes * 2]);
    std::vector<uint8_t> left(ps4::kEyeBayerBytes), right(ps4::kEyeBayerBytes), reference(ps4::kEyeBayerBytes * 2);
    LARGE_INTEGER freq{}; QueryPerformanceFrequency(&freq);
    auto elapsed = [&](bool direct) {
        LARGE_INTEGER begin{}, end{}; QueryPerformanceCounter(&begin);
        for (int i = 0; i < 200; ++i)
        {
            if (direct)
            { AcquiredFrame frame{}; view._lastSeq = 0; view.AcquireFrame(frame, 0, false); }
            else
            {
                source->WaitNewer(0, 0);
                source->SnapshotPlanes(view._bayerL.data(), view._bayerR.data());
                memcpy(left.data(), view._bayerL.data(), left.size());
                memcpy(right.data(), view._bayerR.data(), right.size());
                for (uint32_t y = 0; y < geom.eyeH; ++y)
                {
                    memcpy(reference.data() + size_t(y) * geom.BandBytes() * 2,
                           left.data() + size_t(y) * geom.BandBytes(), geom.BandBytes());
                    memcpy(reference.data() + size_t(y) * geom.BandBytes() * 2 + geom.BandBytes(),
                           right.data() + size_t(y) * geom.BandBytes(), geom.BandBytes());
                }
            }
        }
        QueryPerformanceCounter(&end);
        return double(end.QuadPart - begin.QuadPart) * 1000 / double(freq.QuadPart) / 200;
    };
    elapsed(false); elapsed(true);
    const double oldMs = elapsed(false), directMs = elapsed(true);
    Check(memcmp(reference.data(), view._yuy2Out.get(), reference.size()) == 0,
          "full-size SBS benchmark preserves output identity");
    printf("[INFO] Offline 2560x800 SBS, no flips: reference %.3f ms/frame; direct %.3f ms/frame\n", oldMs, directMs);
}

int main()
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    TestRates(); TestRecording(); TestPs4(); TestAudioStatus();
    MeasureSbs();
    Check(audiostatus::Name(1) != audiostatus::Name(2), "physical microphone owners have distinct status mapping identities");
    if (SUCCEEDED(com)) CoUninitialize();
    printf("host: %d failures\n", testFailures);
    return testFailures ? 1 : 0;
}
