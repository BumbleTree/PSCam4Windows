#pragma once
//
// Ps4Demosaic — the RAW-mode (PS4_RAW=1) image pipeline. The default ISP mode
// gets finished YUY2 from the OV580 and never calls this.
//
// The OmniVision OV9713 behind the OV580 bridge delivers each eye as a raw
// Bayer mosaic (GRBG phase, stored 16-bit little-endian — confirmed on hardware:
// BGGR turns the whole scene green, GRBG gives neutral walls + natural colour).
// To present a full-resolution colour image this runs an ISP-style pipeline
// rather than 2x2 binning:
//
//   raw Bayer (1280x800, GRBG)
//     -> full-resolution bilinear demosaic            (no resolution loss)
//     -> grey-world white balance + percentile auto-level (neutral, correctly bright)
//     -> gamma 2.2 encode                             (linear sensor -> sRGB-ish)
//     -> BT.601 RGB->YUY2                             (FrameBus canonical format)
//
// White-balance / exposure gains are estimated per frame from a sub-sampled grid
// and SMOOTHED with a temporal IIR (Ps4ToneState) so the picture never flickers
// or "breathes" — the single most visible artefact of naive per-frame
// auto-contrast. RAW mode configures no sensor exposure register, so this
// software auto-level is what keeps the otherwise washed-out / dark raw looking
// correct; it is built to degrade gracefully whether the raw is 10-bit
// right-justified or scaled into the full 16-bit range.
//
// The hot path (demosaic + tone + pack) is fused into one row-pair sweep and
// drives every output pixel through a per-frame, per-channel lookup table, so
// the only per-pixel float work is the bilinear average. Comfortably real-time
// for two 1280x800 eyes at 60 fps.
//
#include <cstdint>

namespace ps4 {

// Persistent, per-view tone state. Holds the smoothed per-channel gains and
// black level between frames so auto-white-balance / auto-exposure converge
// gently instead of jumping every frame. One instance per Ps4ViewDevice (each
// eye/view tones independently). Zero-initialised state means "not yet
// converged" — the first frame snaps straight to its measurement.
struct Ps4ToneState
{
    float gainR = 0.0f;   // effective per-channel gain (white balance * exposure)
    float gainG = 0.0f;
    float gainB = 0.0f;
    float black = 0.0f;   // smoothed black level (raw units)
    bool  primed = false; // false until the first frame seeds the state

    void Reset() { gainR = gainG = gainB = black = 0.0f; primed = false; }
};

// Demosaic one GRBG Bayer plane to a YUY2 image of the SAME dimensions, applying
// white balance / auto-level / gamma via `tone` (updated in place).
//
//   bayer16 : w*h little-endian 16-bit samples, GRBG phase at (0,0).
//   w, h    : MUST be even (Bayer 2x2 quads + YUY2 2-pixel macropixels).
//   yuy2    : caller-owned, w*h*2 bytes.
//
// flipH / flipV mirror the output (software flip; RAW mode programs no sensor
// flip registers). The Bayer phase is taken from the source position, so
// colours stay right under the mirror.
void DemosaicGrbgToYuy2(const uint8_t* bayer16, uint32_t w, uint32_t h,
                        uint8_t* yuy2, Ps4ToneState& tone,
                        bool flipH = false, bool flipV = false);

} // namespace ps4
