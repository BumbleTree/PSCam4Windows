#pragma once
//
// Ps3EyePreviewSource — the PS3-Eye implementation of ICameraPreviewSource.
//
// Composes:
//   * framebus::Reader  (FILE_MAP_READ seqlock consumer; multi-reader safe,
//                        cannot affect the writer or any other reader such as
//                        the DLL serving OBS/Discord)
//   * controlbus::ActivityAge() (read-only query used for the "in use" badge)
//
// Lives entirely on the preview worker thread; no shared mutable state.
//
#include "ICameraPreviewSource.h"
#include "../common/FrameBus.h"

class Ps3EyePreviewSource final : public ICameraPreviewSource
{
public:
    bool TryOpen(int cameraIndex) override;
    bool IsOpen() const override;
    void Close() override;
    bool ReadFormat(uint32_t& width, uint32_t& height, uint32_t& fps) override;
    void WaitFrame(DWORD timeoutMs) const override;
    LONG64 TryReadNewer(uint8_t* dst, uint32_t dstBytes, LONG64 lastFrameId) override;
    bool IsCameraInUse(uint32_t idleTimeoutMs) const override;

private:
    framebus::Reader _reader;
    int _cameraIndex = 0;
};
