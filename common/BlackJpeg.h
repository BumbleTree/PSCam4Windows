#pragma once
#include <cstdint>
#include <memory>
#include <vector>
#include "../third_party/libjpeg-turbo/include/turbojpeg.h"

namespace jpeg {
// Encode a solid-black width x height JFIF into `result`: the frame an MJPEG
// client is served when no camera JPEG is available. False on bad dimensions or
// encoder/allocation failure.
inline bool Black(uint32_t width, uint32_t height, std::vector<uint8_t>& result)
{
    if (!width || !height || width > 4096 || height > 4096) return false;
    struct Compressor { tjhandle h = tjInitCompress(); ~Compressor() { if (h) tjDestroy(h); } };
    struct Output { unsigned char* p = nullptr; ~Output() { tjFree(p); } };
    Compressor compressor;
    Output output;
    if (!compressor.h) return false;
    try
    {
        std::vector<uint8_t> rgb(static_cast<size_t>(width) * height * 3, 0);
        unsigned long size = 0;
        if (tjCompress2(compressor.h, rgb.data(), static_cast<int>(width), 0,
                        static_cast<int>(height), TJPF_RGB, &output.p, &size,
                        TJSAMP_420, 75, 0) != 0) return false;
        result.assign(output.p, output.p + size);
        return true;
    }
    catch (const std::bad_alloc&) { return false; }
}
} // namespace jpeg
