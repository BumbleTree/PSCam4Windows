#pragma once
//
// Shared YUY2 helpers: black fill, 2x scaling, and NV12 conversion.
//
// Pure functions over buffers, kept out of the DLL so tests can link them.
//
#include <cstddef>
#include <cstdint>
#include <cstring>   // memcpy (SWAR loads/stores, row replication)

namespace yuv {

// YUY2 black: Y=0x10, U=V=0x80. One 32-bit word covers two pixels.
constexpr uint32_t kBlackWord = 0x80108010u;

// Fill a w x h YUY2 image with black, writing at most capBytes.
// capBytes defaults to the image size; pass a region size to clamp. dst must be
// 4-byte aligned.
inline void FillBlack(uint8_t* dst, uint32_t w, uint32_t h, size_t capBytes = 0)
{
    if (!dst)
        return;
    const size_t imageBytes = static_cast<size_t>(w) * h * 2;
    const size_t limit      = capBytes ? (capBytes < imageBytes ? capBytes : imageBytes)
                                       : imageBytes;
    uint32_t* p = reinterpret_cast<uint32_t*>(dst);
    const size_t words = limit / 4;             // 4 bytes per 2 pixels
    for (size_t i = 0; i < words; ++i)
        p[i] = kBlackWord;
}

// ---- SWAR helpers (x64 little-endian) -------------------------------------

inline uint64_t Load64(const uint8_t* p)
{
    uint64_t v;
    memcpy(&v, p, sizeof(v));
    return v;
}

inline void Store64(uint8_t* p, uint64_t v)
{
    memcpy(p, &v, sizeof(v));
}

// Rounding-up byte-wise average of 8 byte lanes: per byte, (a + b + 1) >> 1.
// Identity: avg_up(a,b) = (a | b) - (((a ^ b) >> 1) & 0x7f per lane).
inline uint64_t AvgBytes8(uint64_t a, uint64_t b)
{
    return (a | b) - (((a ^ b) >> 1) & 0x7f7f7f7f7f7f7f7full);
}

// Bilinear 2x upscale in the YUY2 domain (e.g. 320x240 -> 640x480).
//
// Pass 1 writes every even output row: luma and chroma are doubled with the
// in-between samples linearly interpolated from their horizontal neighbours
// (edge samples replicate). Pass 2 fills the odd rows as the byte-wise mean
// of the two adjacent even rows — in YUY2 every byte column holds the same
// component on every row, so a byte-wise average IS a true vertical lerp.
inline void UpscaleYuy2_2x(const uint8_t* src, uint8_t* dst, uint32_t srcW, uint32_t srcH)
{
    const uint32_t srcStride = srcW * 2;
    const uint32_t dstStride = srcW * 4;        // (srcW * 2 px) * 2 bytes
    const uint32_t srcPairs  = srcW / 2;        // YUYV macropixels per row

    for (uint32_t y = 0; y < srcH; ++y)
    {
        const uint8_t* s = src + y * srcStride;
        uint8_t* d = dst + (y * 2) * dstStride;

        for (uint32_t i = 0; i < srcPairs; ++i)
        {
            const uint32_t y0 = s[i * 4 + 0], u0 = s[i * 4 + 1];
            const uint32_t y1 = s[i * 4 + 2], v0 = s[i * 4 + 3];
            const bool last = (i + 1 == srcPairs);
            const uint32_t y2 = last ? y1 : s[i * 4 + 4];
            const uint32_t u1 = last ? u0 : s[i * 4 + 5];
            const uint32_t v1 = last ? v0 : s[i * 4 + 7];

            uint8_t* o = d + i * 8;
            o[0] = static_cast<uint8_t>(y0);
            o[1] = static_cast<uint8_t>(u0);
            o[2] = static_cast<uint8_t>((y0 + y1 + 1) >> 1);
            o[3] = static_cast<uint8_t>(v0);
            o[4] = static_cast<uint8_t>(y1);
            o[5] = static_cast<uint8_t>((u0 + u1 + 1) >> 1);
            o[6] = static_cast<uint8_t>((y1 + y2 + 1) >> 1);
            o[7] = static_cast<uint8_t>((v0 + v1 + 1) >> 1);
        }
    }

    // In YUY2 every byte column holds the same component on every row, so a
    // byte-wise average IS a true vertical lerp; do it 8 bytes per op.
    for (uint32_t y = 0; y + 1 < srcH; ++y)
    {
        const uint8_t* a = dst + (y * 2) * dstStride;
        const uint8_t* b = a + 2 * dstStride;
        uint8_t* o = dst + (y * 2 + 1) * dstStride;
        uint32_t x = 0;
        for (; x + 8 <= dstStride; x += 8)
            Store64(o + x, AvgBytes8(Load64(a + x), Load64(b + x)));
        for (; x < dstStride; ++x)
            o[x] = static_cast<uint8_t>((a[x] + b[x] + 1) >> 1);
    }
    memcpy(dst + (srcH * 2 - 1) * dstStride, dst + (srcH * 2 - 2) * dstStride, dstStride);
}

// 2x2 box-filter downscale in the YUY2 domain (e.g. 640x480 -> 320x240).
// Every output sample is the mean of the four source samples it covers,
// which avoids the aliasing of point sampling.
inline void DownscaleYuy2_2x(const uint8_t* src, uint8_t* dst, uint32_t srcW, uint32_t srcH)
{
    const uint32_t srcStride = srcW * 2;
    const uint32_t dstStride = srcW;            // (srcW / 2 px) * 2 bytes
    const uint32_t dstPairs  = srcW / 4;        // output macropixels per row

    for (uint32_t y = 0; y < srcH / 2; ++y)
    {
        const uint8_t* r0 = src + (y * 2) * srcStride;
        const uint8_t* r1 = r0 + srcStride;
        uint8_t* d = dst + y * dstStride;

        for (uint32_t i = 0; i < dstPairs; ++i)
        {
            const uint8_t* s0 = r0 + i * 8;     // two source macropixels...
            const uint8_t* s1 = r1 + i * 8;     // ...on each of two rows
            uint8_t* o = d + i * 4;
            o[0] = static_cast<uint8_t>((s0[0] + s0[2] + s1[0] + s1[2] + 2) >> 2);  // Y
            o[1] = static_cast<uint8_t>((s0[1] + s0[5] + s1[1] + s1[5] + 2) >> 2);  // U
            o[2] = static_cast<uint8_t>((s0[4] + s0[6] + s1[4] + s1[6] + 2) >> 2);  // Y
            o[3] = static_cast<uint8_t>((s0[3] + s0[7] + s1[3] + s1[7] + 2) >> 2);  // V
        }
    }
}

// The single definition of what DeliverSample can produce from a bus frame.
// Both the advertised type list and the delivery path consult this, so they
// cannot disagree: advertising a size delivery cannot produce yields a client
// that only ever gets black, at full frame rate, with nothing to say why.
enum class ScalePlan
{
    Unsupported = 0,
    Direct,        // sizes match: straight copy, no scaling
    Upscale2x,     // UpscaleYuy2_2x   -- 320x240 -> 640x480 only
    Downscale2x,   // DownscaleYuy2_2x -- any exact 2:1 halving
};

inline ScalePlan ScalePlanFor(uint32_t busW, uint32_t busH, uint32_t outW, uint32_t outH)
{
    if (outW == busW && outH == busH)
        return ScalePlan::Direct;
    if (busW == 320 && busH == 240 && outW == 640 && outH == 480)
        return ScalePlan::Upscale2x;
    // Any exact 2:1 halving. Width must divide by 4, not 2: DownscaleYuy2_2x
    // walks whole YUY2 macropixel pairs (dstPairs = srcW / 4).
    if (outW * 2 == busW && outH * 2 == busH && (busW % 4) == 0 && (busH % 2) == 0)
        return ScalePlan::Downscale2x;
    return ScalePlan::Unsupported;
}


// YUY2 (4:2:2) -> NV12 (4:2:0). Luma is copied; each output chroma sample is
// the mean of the two source rows it replaces — the correct 4:2:0 downsample.
//
// Inner loop works on 8 source bytes per row (4 pixels / 2 macropixels): one
// SWAR average yields both rows' chroma means at once (lumas in the averaged
// word are simply ignored), and the lumas are picked out of the loaded words.
inline void Yuy2ToNv12(const uint8_t* src, uint8_t* dst, uint32_t w, uint32_t h)
{
    const uint32_t srcStride = w * 2;
    uint8_t* dstY  = dst;
    uint8_t* dstUV = dst + static_cast<size_t>(w) * h;

    for (uint32_t y = 0; y < h; y += 2)
    {
        const uint8_t* p0 = src + y * srcStride;
        const uint8_t* p1 = p0 + srcStride;
        uint8_t* y0 = dstY + y * w;
        uint8_t* y1 = y0 + w;
        uint8_t* uv = dstUV + (y / 2) * w;

        uint32_t x = 0;
        for (; x + 4 <= w; x += 4)
        {
            const uint64_t a   = Load64(p0);   // Y0 U0 Y1 V0 Y2 U1 Y3 V1
            const uint64_t b   = Load64(p1);
            const uint64_t avg = AvgBytes8(a, b);

            y0[0] = static_cast<uint8_t>(a);
            y0[1] = static_cast<uint8_t>(a >> 16);
            y0[2] = static_cast<uint8_t>(a >> 32);
            y0[3] = static_cast<uint8_t>(a >> 48);
            y1[0] = static_cast<uint8_t>(b);
            y1[1] = static_cast<uint8_t>(b >> 16);
            y1[2] = static_cast<uint8_t>(b >> 32);
            y1[3] = static_cast<uint8_t>(b >> 48);
            uv[0] = static_cast<uint8_t>(avg >> 8);   // U
            uv[1] = static_cast<uint8_t>(avg >> 24);  // V
            uv[2] = static_cast<uint8_t>(avg >> 40);  // U
            uv[3] = static_cast<uint8_t>(avg >> 56);  // V

            p0 += 8; p1 += 8; y0 += 4; y1 += 4; uv += 4;
        }
        for (; x < w; x += 2)
        {
            y0[0] = p0[0];
            y0[1] = p0[2];
            y1[0] = p1[0];
            y1[1] = p1[2];
            uv[0] = static_cast<uint8_t>((p0[1] + p1[1] + 1) >> 1);
            uv[1] = static_cast<uint8_t>((p0[3] + p1[3] + 1) >> 1);
            p0 += 4; p1 += 4; y0 += 2; y1 += 2; uv += 2;
        }
    }
}

} // namespace yuv
