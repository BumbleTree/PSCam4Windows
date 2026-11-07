#include <windows.h>
#include <objbase.h>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <libusb.h>
#include "TestSupport.h"
#include "../host/ICameraDevice.h"
#define private public
#include "../transports/usb_iso/EyeToyDevice.h"
#undef private
static int TestControl(libusb_device_handle*, uint8_t, uint8_t, uint16_t, uint16_t,
                       unsigned char*, uint16_t length, unsigned int) { return length; }
static int TestAlt(libusb_device_handle*, int, int) { return 0; }
static int TestRelease(libusb_device_handle*, int) { return 0; }
static int TestCancel(libusb_transfer*) { return 0; }
static void TestClose(libusb_device_handle*) {}
#define libusb_control_transfer TestControl
#define libusb_set_interface_alt_setting TestAlt
#define libusb_release_interface TestRelease
#define libusb_cancel_transfer TestCancel
#define libusb_close TestClose
#include "../transports/usb_iso/EyeToyDevice.cpp"
#undef libusb_control_transfer
#undef libusb_set_interface_alt_setting
#undef libusb_release_interface
#undef libusb_cancel_transfer
#undef libusb_close

static void TestJpeg(int width, int height, int subsampling, bool accepted)
{
    EyeToyDevice device;
    device._width = width; device._height = height;
    const size_t imageBytes = static_cast<size_t>(width) * height * 2;
    device._yuy2Out.reset(new uint8_t[imageBytes + 16]);
    std::fill_n(device._yuy2Out.get(), imageBytes + 16, static_cast<uint8_t>(0x42));
    device._tjDecomp = tjInitDecompress();
    tjhandle compressor = tjInitCompress();
    const bool gray = subsampling == TJSAMP_GRAY;
    const std::vector<BYTE> pixels(static_cast<size_t>(width) * height * (gray ? 1 : 3), 128);
    BYTE* jpeg = nullptr; unsigned long length = 0;
    const int result = tjCompress2(compressor, pixels.data(), width, 0, height,
                                   gray ? TJPF_GRAY : TJPF_RGB, &jpeg, &length, subsampling, 90, 0);
    Check(result == 0, "valid JPEG fixture encoded with the vendored library");
    if (!result)
        device.DecodeJpegToYuy2(jpeg, static_cast<uint32_t>(length));
    const bool untouched = std::all_of(device._yuy2Out.get(), device._yuy2Out.get() + imageBytes,
                                       [](BYTE b) { return b == 0x42; });
    Check(accepted ? !untouched : untouched, accepted ? "supported color JPEG decodes" : "unsupported JPEG preserves previous output");
    Check(std::all_of(device._yuy2Out.get() + imageBytes, device._yuy2Out.get() + imageBytes + 16,
                      [](BYTE b) { return b == 0x42; }), "JPEG decode stays inside the output buffer");
    tjFree(jpeg); tjDestroy(compressor);
}

int main()
{
    EyeToyDevice pending;
    pending._h = reinterpret_cast<libusb_device_handle*>(uintptr_t(1));
    pending._streaming = true;
    pending._xfers.push_back(libusb_alloc_transfer(0));
    pending._xferBufs.assign(1, std::vector<uint8_t>(128, 0x42));
    pending._xfersInFlight.store(1);
    std::atomic<bool> stopped{false};
    std::thread stop([&] { pending.Stop(); stopped.store(true); });
    Sleep(30);
    Check(!stopped.load(), "EyeToy teardown retains device and transfer buffers during delayed cancellation");
    pending._xfersInFlight.store(0);
    stop.join();
    Check(stopped.load() && pending._xfers.empty() && pending._xferBufs.empty() && !pending._h,
          "EyeToy frees asynchronous resources only after cancellation completes");
    TestJpeg(320, 240, TJSAMP_GRAY, false);
    TestJpeg(640, 480, TJSAMP_GRAY, false);
    for (int subsampling : { TJSAMP_444, TJSAMP_422, TJSAMP_420, TJSAMP_440 })
        TestJpeg(320, 240, subsampling, true);
    TestJpeg(319, 239, TJSAMP_420, false);
    printf("jpeg: %d failures\n", testFailures);
    return testFailures ? 1 : 0;
}
