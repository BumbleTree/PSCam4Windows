#include "Ps4Demosaic.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ps4 {
namespace {

// --- tuning ----------------------------------------------------------------
// Target mid-level (linear, 0..1) that each channel's mean is driven toward by
// the auto-level. ~0.42 lands a typical scene's average around 168/255 after
// gamma — bright but not blown.
constexpr float kTargetLin   = 0.42f;
constexpr float kGamma       = 2.2f;          // sensor is linear; encode to sRGB-ish
constexpr float kSmooth      = 0.10f;         // IIR factor for gains/levels (per frame)
constexpr float kWbRatioMin  = 0.40f;         // clamp R,B gain relative to G (avoid
constexpr float kWbRatioMax  = 2.50f;         //   over-correcting a genuinely tinted scene)
constexpr int   kLutSize     = 1024;          // per-channel tone LUT entries
// Black/white points come from luma PERCENTILES, not absolute min/max, so a band
// whose data is bunched into a narrow range (e.g. the two OV9713 sensors run at
// different exposures — one eye can land washed-out at 0.45..0.72) is still
// stretched to full contrast, and a few hot/dead pixels don't define the range.
constexpr float kBlackPct    = 0.02f;         // 2nd percentile -> black point
constexpr float kWhitePct    = 0.98f;         // 98th percentile -> white point
constexpr int   kHistBins    = 256;           // over the 16-bit sample range (v>>8)

inline int Rd(const uint8_t* row, uint32_t col)
{
    return row[col * 2] | (row[col * 2 + 1] << 8);   // 16-bit little-endian sample
}

// Bilinear BGGR demosaic of one source pixel (x,y). row{m1,0,p1} are byte
// pointers to clamped source rows y-1, y, y+1; columns are clamped here. Returns
// the three raw (sensor-unit) channel values. Phase is taken from absolute
// (x&1,y&1) so the Bayer mosaic stays aligned to the sensor regardless of any
// crop/flip the caller applies.
inline void DemosaicPixel(const uint8_t* rowm1, const uint8_t* row0, const uint8_t* rowp1,
                          uint32_t x, uint32_t w, int& R, int& G, int& B, int yOdd)
{
    const uint32_t xm1 = x ? x - 1 : 0;
    const uint32_t xp1 = (x + 1 < w) ? x + 1 : w - 1;
    const int xOdd = x & 1;

    if (!yOdd)
    {
        if (!xOdd)   // B site (even,even)
        {
            B = Rd(row0, x);
            G = (Rd(row0, xm1) + Rd(row0, xp1) + Rd(rowm1, x) + Rd(rowp1, x)) >> 2;
            R = (Rd(rowm1, xm1) + Rd(rowm1, xp1) + Rd(rowp1, xm1) + Rd(rowp1, xp1)) >> 2;
        }
        else         // G site on a B row: H-neighbours B, V-neighbours R
        {
            G = Rd(row0, x);
            B = (Rd(row0, xm1) + Rd(row0, xp1)) >> 1;
            R = (Rd(rowm1, x) + Rd(rowp1, x)) >> 1;
        }
    }
    else
    {
        if (!xOdd)   // G site on an R row: H-neighbours R, V-neighbours B
        {
            G = Rd(row0, x);
            R = (Rd(row0, xm1) + Rd(row0, xp1)) >> 1;
            B = (Rd(rowm1, x) + Rd(rowp1, x)) >> 1;
        }
        else         // R site (odd,odd)
        {
            R = Rd(row0, x);
            G = (Rd(row0, xm1) + Rd(row0, xp1) + Rd(rowm1, x) + Rd(rowp1, x)) >> 2;
            B = (Rd(rowm1, xm1) + Rd(rowm1, xp1) + Rd(rowp1, xm1) + Rd(rowp1, xp1)) >> 2;
        }
    }
}

inline uint8_t ClampU8(int v) { return (uint8_t)(v < 0 ? 0 : (v > 255 ? 255 : v)); }

} // namespace

void DemosaicBggrToYuy2(const uint8_t* bayer16, uint32_t w, uint32_t h,
                        uint8_t* yuy2, Ps4ToneState& tone,
                        bool flipH, bool flipV)
{
    if (!bayer16 || !yuy2 || w < 2 || h < 2)
        return;
    const size_t rowStride = (size_t)w * 2;   // bytes per Bayer row

    // ---- Pass 1: sub-sampled statistics (every 4th 2x2 quad) ---------------
    // Accumulate per-channel means plus a luma histogram, straight off the raw
    // Bayer sites — no demosaic needed for statistics. The histogram (over green,
    // the dominant luma term) yields percentile black/white points below.
    double sumR = 0, sumG = 0, sumB = 0;
    long   n = 0;
    uint32_t hist[kHistBins] = {};
    for (uint32_t y = 0; y + 1 < h; y += 8)
    {
        const uint8_t* r0 = bayer16 + (size_t)y * rowStride;
        const uint8_t* r1 = r0 + rowStride;
        for (uint32_t x = 0; x + 1 < w; x += 8)
        {
            const int g0 = Rd(r0, x);        // GRBG: G at (x,y)
            const int rr = Rd(r0, x + 1);    //       R at (x+1,y)
            const int b  = Rd(r1, x);        //       B at (x,y+1)
            const int g1 = Rd(r1, x + 1);    //       G at (x+1,y+1)
            const int g  = (g0 + g1) >> 1;
            sumB += b; sumG += g; sumR += rr; ++n;
            ++hist[(g >> 8) & (kHistBins - 1)];   // 16-bit -> 256 bins
        }
    }
    if (n == 0) { memset(yuy2, 0, (size_t)w * h * 2); return; }

    const float meanR = (float)(sumR / n);
    const float meanG = (float)(sumG / n);
    const float meanB = (float)(sumB / n);

    // Percentile black/white from the luma histogram (robust to outliers and to a
    // narrow-range, washed-out sensor). Bin centre -> 16-bit sample units.
    auto pctBin = [&](float p) -> int {
        const long want = (long)(p * n);
        long acc = 0;
        for (int i = 0; i < kHistBins; ++i) { acc += hist[i]; if (acc >= want) return i; }
        return kHistBins - 1;
    };
    const float loP = (float)((pctBin(kBlackPct) << 8) + 128);
    const float hiP = (float)((pctBin(kWhitePct) << 8) + 128);

    // Smooth the black level, then derive a combined white-balance + exposure gain
    // per channel: drive each channel's (mean - black) to kTargetLin.
    if (!tone.primed)
        tone.black = loP;
    tone.black += kSmooth * (loP - tone.black);
    const float black = tone.black;

    float white = hiP;
    if (white < black + 1.0f) white = black + 1.0f;

    auto chanGain = [&](float mean) {
        const float denom = std::max(mean - black, 1.0f);
        float g = kTargetLin / denom;
        // Cap amplification so a near-black scene cannot blow sensor noise up to
        // white (limits gain to mapping the brightest sample to ~8x target).
        const float gMax = 8.0f / std::max(white - black, 1.0f);
        return std::min(g, gMax);
    };
    float gR = chanGain(meanR), gG = chanGain(meanG), gB = chanGain(meanB);

    // Keep the colour correction sane: clamp R/B gains relative to G so a truly
    // tinted scene is balanced, not forced grey.
    gR = std::min(std::max(gR, gG * kWbRatioMin), gG * kWbRatioMax);
    gB = std::min(std::max(gB, gG * kWbRatioMin), gG * kWbRatioMax);

    if (!tone.primed)
    {
        tone.gainR = gR; tone.gainG = gG; tone.gainB = gB;
        tone.primed = true;
    }
    else
    {
        tone.gainR += kSmooth * (gR - tone.gainR);
        tone.gainG += kSmooth * (gG - tone.gainG);
        tone.gainB += kSmooth * (gB - tone.gainB);
    }

    // Index shift so the LUT spans 0..white at full precision (full 10-bit
    // resolution when the raw is 10-bit; graceful coverage if it is scaled into
    // the wider 16-bit range).
    int shift = 0;
    while (((int)white >> shift) >= kLutSize) ++shift;

    // ---- Build per-channel tone LUTs: raw -> gamma-encoded 8-bit ------------
    const float invGamma = 1.0f / kGamma;
    uint8_t lutR[kLutSize], lutG[kLutSize], lutB[kLutSize];
    auto buildLut = [&](uint8_t* lut, float gain) {
        for (int i = 0; i < kLutSize; ++i)
        {
            const float raw = (float)(i << shift);
            float lin = (raw - black) * gain;
            lin = lin < 0.0f ? 0.0f : (lin > 1.0f ? 1.0f : lin);
            lut[i] = (uint8_t)(std::pow(lin, invGamma) * 255.0f + 0.5f);
        }
    };
    buildLut(lutR, tone.gainR);
    buildLut(lutG, tone.gainG);
    buildLut(lutB, tone.gainB);
    const int idxMax = kLutSize - 1;

    // ---- Pass 2: output-driven demosaic + tone + BT.601 YUY2 pack ----------
    // Iterating OUTPUT pixels lets flipH/flipV fall out of the source mapping
    // with no temp buffer. Adjacent output pixels share one chroma sample (YUY2).
    for (uint32_t oy = 0; oy < h; ++oy)
    {
        const uint32_t sy = flipV ? (h - 1 - oy) : oy;
        const uint32_t sym1 = sy ? sy - 1 : 0;
        const uint32_t syp1 = (sy + 1 < h) ? sy + 1 : h - 1;
        const uint8_t* rowm1 = bayer16 + (size_t)sym1 * rowStride;
        const uint8_t* row0  = bayer16 + (size_t)sy   * rowStride;
        const uint8_t* rowp1 = bayer16 + (size_t)syp1 * rowStride;
        // Sensor phase is GRBG (G R / B G at (0,0)) — confirmed on hardware: BGGR
        // turns the whole scene green, GRBG gives neutral walls + natural colour.
        // DemosaicPixel implements BGGR site logic, and GRBG == BGGR with the row
        // phase inverted, so feed it the inverted source-row parity.
        const int yOdd = (sy & 1) ^ 1;

        uint8_t* o = yuy2 + (size_t)oy * w * 2;
        for (uint32_t ox = 0; ox < w; ox += 2)
        {
            const uint32_t sx0 = flipH ? (w - 1 - ox)       : ox;
            const uint32_t sx1 = flipH ? (w - 1 - (ox + 1)) : (ox + 1);

            int R0, G0, B0, R1, G1, B1;
            DemosaicPixel(rowm1, row0, rowp1, sx0, w, R0, G0, B0, yOdd);
            DemosaicPixel(rowm1, row0, rowp1, sx1, w, R1, G1, B1, yOdd);

            const int r0 = lutR[std::min(R0 >> shift, idxMax)];
            const int g0 = lutG[std::min(G0 >> shift, idxMax)];
            const int b0 = lutB[std::min(B0 >> shift, idxMax)];
            const int r1 = lutR[std::min(R1 >> shift, idxMax)];
            const int g1 = lutG[std::min(G1 >> shift, idxMax)];
            const int b1 = lutB[std::min(B1 >> shift, idxMax)];

            // BT.601 limited-range RGB->YCbCr (matches the FrameBus YUY2 black
            // level Y=16, U/V=128 and the EyeToy decode constants).
            const int y0 = 16  + ((66 * r0 + 129 * g0 + 25 * b0 + 128) >> 8);
            const int y1 = 16  + ((66 * r1 + 129 * g1 + 25 * b1 + 128) >> 8);
            const int rA = (r0 + r1) >> 1, gA = (g0 + g1) >> 1, bA = (b0 + b1) >> 1;
            const int u  = 128 + ((-38 * rA - 74 * gA + 112 * bA + 128) >> 8);
            const int v  = 128 + ((112 * rA - 94 * gA - 18 * bA + 128) >> 8);

            o[0] = ClampU8(y0);
            o[1] = ClampU8(u);
            o[2] = ClampU8(y1);
            o[3] = ClampU8(v);
            o += 4;
        }
    }
}

} // namespace ps4
