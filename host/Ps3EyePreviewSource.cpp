#include "Ps3EyePreviewSource.h"

#include "../common/ControlBus.h"

bool Ps3EyePreviewSource::TryOpen(int cameraIndex)
{
    _cameraIndex = cameraIndex;
    return _reader.TryOpen(cameraIndex);
}

bool Ps3EyePreviewSource::IsOpen() const
{
    return _reader.IsOpen();
}

void Ps3EyePreviewSource::Close()
{
    _reader.Close();
}

bool Ps3EyePreviewSource::ReadFormat(uint32_t& width, uint32_t& height, uint32_t& fps)
{
    framebus::Header h{};
    if (!_reader.ReadFormat(h))
        return false;
    width  = h.width;
    height = h.height;
    fps    = h.fpsNum / (h.fpsDen ? h.fpsDen : 1);
    return true;
}

void Ps3EyePreviewSource::WaitFrame(DWORD timeoutMs) const
{
    // Do not wait on the shared auto-reset event, because doing so would
    // steal wakeup signals from the virtual camera DLL (which is also waiting
    // on it). Instead, use a simple Sleep to poll the lock-free seqlock.
    Sleep(timeoutMs < 16 ? timeoutMs : 16);
}

LONG64 Ps3EyePreviewSource::TryReadNewer(uint8_t* dst, uint32_t dstBytes, LONG64 lastFrameId)
{
    return _reader.TryReadNewer(dst, dstBytes, lastFrameId);
}

bool Ps3EyePreviewSource::IsCameraInUse(uint32_t idleTimeoutMs) const
{
    // Read-only query: opens the ControlBus section FILE_MAP_READ, reads the
    // keepalive tick once, closes. MAXULONGLONG (section absent / never
    // stamped) means no external client is consuming.
    const ULONGLONG age = controlbus::ActivityAge(_cameraIndex);
    return age != MAXULONGLONG && age < idleTimeoutMs;
}
