#pragma once
//
// PS4 microphone decimation/resampling, independent of USB capture so offline
// tests exercise the same arithmetic as the live audio path. DspConfig defaults
// define the production signal path; alternatives require sample-level and
// listening checks, not just a successful build.
//
#include <cstdint>
#include <cstring>
#include <math.h>

namespace ps4mic {

struct DspConfig
{
    // ---- shipped defaults: do not change these, add alternatives instead ----
    uint32_t taps       = 64;      // MUST be a power of two (the history ring is masked)
    double   cutoffFrac = 0.35;    // x the decimated rate; 16.97 kHz at 1280x800@60
    enum Window { Blackman = 0, Kaiser = 1 };
    int      window     = Blackman;
    double   kaiserBeta = 8.0;

    // The shipped path truncates in two places: `acc >> 15` floors (a -0.5 LSB
    // DC bias) and the Catmull-Rom `/ 131072` truncates toward zero (so the
    // error changes sign with the signal). Measured to change nothing audible,
    // so this is a correctness tidy and stays opt-in for the identity gate.
    bool     roundNearest = false;

    // ---- high-pass: the one measured quality lever ------------------------
    // This array is ACOUSTICALLY rumble-limited. Measured over three corpora,
    // 39-75 % of its power sits below 30 Hz on quiet content and 22 % even under
    // a loud program, and inter-channel coherence below 100 Hz (0.85-0.91) is
    // HIGHER than at 300 Hz-3 kHz (0.60-0.71) -- long-wavelength sound reaching
    // all four capsules alike, not converter drift. Nothing in the chain removed
    // it. An 80 Hz corner recovers ~5.9 dB of headroom and costs nothing audible.
    //
    // Second-order Butterworth, Q30 coefficients over Q12 state. The precision
    // is not decoration: a naive Q15 one-pole was measured WORSE than no filter
    // at all (THD at -60 dBFS went -44.7 -> -28.1 dB) because its state
    // truncates and limit-cycles. Do not "simplify" this back to one pole.
    bool     highPass     = true;
    double   highPassHz   = 80.0;

    // ---- honour the per-row valid mask ------------------------------------
    // Row header byte 0 is a per-slot valid bitmask, and it is NOT constant: it
    // ramps 2,4,4,6,6,6,6 over rows 0-6 and back down 6..0 across the trailer,
    // identically in 100 % of frames. That is exactly the 64 slots per frame
    // that make up "6400 valid of 6464". Decoding all 8 slots of every row
    // injects those 64 padding samples once per frame, at 60 Hz, scaling with
    // the signal -- audible on speech as a roughness on the boundary.
    //
    // MEASURED WORSE, and left here only so the experiment is repeatable:
    // honouring the mask takes the frame-head step ratio from 2.04x to 3.54x
    // (program) and 5.70x to 8.72x (1 kHz tone), and no bit-to-slot mapping
    // tried cleans the boundary. So byte 0 does NOT mean "this slot is stale".
    // Default OFF. Do not turn it on without new evidence about what it means.
    bool     useValidMask = false;
};

class MicDsp
{
public:
    static const uint32_t kChannels = 4;
    static const uint32_t kMaxTaps  = 256;
    static const uint32_t kRate     = 48000;   // the OUTPUT rate we resample to
    static const uint32_t kMaxEmit  = 16;      // ceiling on outputs from one row

    // `validPerFrame` is how many raw samples per channel a frame really carries
    // -- eyeH*8, NOT rows*8. The raw rate is validPerFrame*fps and the
    // decimated rate is an
    // eighth of that: 6400*60/8 = exactly 48,000 at 1280x800@60.
    void Reset(uint32_t validPerFrame, uint32_t fps, const DspConfig& cfg = DspConfig())
    {
        _cfg   = cfg;
        _taps  = cfg.taps < 8 ? 8 : (cfg.taps > kMaxTaps ? kMaxTaps : cfg.taps);
        _mask  = _taps - 1;

        const double slotRate = (double)validPerFrame * (double)fps;
        const double inRate   = slotRate / 8.0;               // after 8:1
        const double cutoff   = cfg.cutoffFrac * inRate;
        const double kPi      = 3.14159265358979323846;

        double h[kMaxTaps], sum = 0.0;
        for (uint32_t i = 0; i < _taps; ++i)
        {
            const double m = (double)i - (double)(_taps - 1) / 2.0;
            const double x = 2.0 * cutoff / slotRate * m;
            const double sinc = fabs(x) < 1e-12 ? 1.0 : sin(kPi * x) / (kPi * x);
            const double t = 2.0 * kPi * (double)i / (double)(_taps - 1);
            double w;
            if (cfg.window == DspConfig::Kaiser)
            {
                const double r = 2.0 * (double)i / (double)(_taps - 1) - 1.0;
                w = I0(cfg.kaiserBeta * sqrt(1.0 - r * r)) / I0(cfg.kaiserBeta);
            }
            else
                w = 0.42 - 0.5 * cos(t) + 0.08 * cos(2.0 * t);   // Blackman
            h[i] = sinc * w;
            sum += h[i];
        }
        int32_t total = 0;
        for (uint32_t i = 0; i < _taps; ++i)
        {
            _fir[i] = (int32_t)floor(h[i] / sum * 32768.0 + 0.5);
            total += _fir[i];
        }
        _fir[_taps / 2] += 32768 - total;   // exactly unity gain

        memset(_hist, 0, sizeof(_hist));
        memset(_interp, 0, sizeof(_interp));
        memset(_hpX1, 0, sizeof(_hpX1)); memset(_hpX2, 0, sizeof(_hpX2));
        memset(_hpY1, 0, sizeof(_hpY1)); memset(_hpY2, 0, sizeof(_hpY2));
        _histPos   = 0;
        _sinceEmit = 0;
        _phase     = 0;
        _step      = (uint32_t)(inRate * 65536.0 / (double)kRate + 0.5);

        // Butterworth (Q = 1/sqrt2) high-pass at the OUTPUT rate, RBJ cookbook
        // form, normalised by a0 and stored Q30.
        const double w0    = 2.0 * kPi * cfg.highPassHz / (double)kRate;
        const double cw    = cos(w0), sw = sin(w0);
        const double alpha = sw / (2.0 * 0.70710678118654752);
        const double a0    = 1.0 + alpha;
        const double Q30   = 1073741824.0;
        _hpB0 = (int64_t)(((1.0 + cw) / 2.0 / a0) * Q30 + 0.5);
        _hpB1 = (int64_t)((-(1.0 + cw)      / a0) * Q30 - 0.5);
        _hpB2 = _hpB0;
        _hpA1 = (int64_t)((-2.0 * cw        / a0) * Q30 - 0.5);
        _hpA2 = (int64_t)(((1.0 - alpha)    / a0) * Q30 + 0.5);
    }

    // Push one row's raw samples. `validMask` is the row header's byte 0: bit s
    // set means slot s carries a fresh sample. Writes 0..kMaxEmit interleaved
    // 4-channel output frames to `out` and returns how many.
    //
    // Emission is driven by SAMPLES accumulated, not by rows: with the mask
    // honoured a row contributes 0..8 samples, so "one output per row" would
    // resample by a wandering ratio. Eight pushed samples produce one output,
    // wherever in the frame they came from.
    uint32_t PushRow(const int16_t raw[kChannels][8], int16_t* out, uint8_t validMask = 0xff)
    {
        uint32_t emitted = 0;
        const uint8_t m = _cfg.useValidMask ? validMask : (uint8_t)0xff;
        for (uint32_t s = 0; s < 8; ++s)
        {
            if (!((m >> s) & 1))
                continue;                      // padding slot: not audio
            const uint32_t h = _histPos & _mask;
            for (uint32_t c = 0; c < kChannels; ++c)
                _hist[c][h] = raw[c][s];
            _histPos = (_histPos + 1) & _mask;
            if (++_sinceEmit < 8)
                continue;
            _sinceEmit = 0;
            emitted += Emit(out + (size_t)emitted * kChannels);
        }
        return emitted < kMaxEmit ? emitted : kMaxEmit;
    }

    // One decimated sample: FIR over the history, then the fractional resample.
    // _histPos points at the OLDEST sample, which is tap 0.
    uint32_t Emit(int16_t* out)
    {
        for (uint32_t c = 0; c < kChannels; ++c)
        {
            int64_t acc = 0;
            for (uint32_t t = 0; t < _taps; ++t)
                acc += (int64_t)_fir[t] * _hist[c][(_histPos + t) & _mask];
            int32_t* w = _interp[c];
            w[0] = w[1]; w[1] = w[2]; w[2] = w[3];
            w[3] = _cfg.roundNearest ? (int32_t)((acc + 16384) >> 15)
                                     : (int32_t)(acc >> 15);
        }

        // Resample the decimated stream -> kRate. Catmull-Rom across the window,
        // interpolating between w[1] and w[2]; phase is Q16.
        uint32_t emitted = 0;
        while (_phase < 65536u)
        {
            const int64_t t = (int64_t)_phase;                 // Q16 in [0,1)
            for (uint32_t c = 0; c < kChannels; ++c)
            {
                const int64_t p0 = _interp[c][0], p1 = _interp[c][1];
                const int64_t p2 = _interp[c][2], p3 = _interp[c][3];
                const int64_t a = 3 * (p1 - p2) + p3 - p0;
                const int64_t b = 2 * p0 - 5 * p1 + 4 * p2 - p3;
                const int64_t c1 = p2 - p0;
                int64_t v;
                if (_cfg.roundNearest)
                {
                    const int64_t inner = (((a * t + 32768) >> 16) + b);
                    const int64_t quad  = ((inner * t + 32768) >> 16) + c1;
                    v = p1 + (quad * t + 65536) / 131072;
                }
                else
                    v = p1 + (((((a * t) >> 16) + b) * t >> 16) + c1) * t / 131072;

                if (_cfg.highPass)
                {
                    // Q12 state, Q30 coefficients, rounded at both shifts.
                    const int64_t xq = v << 12;
                    const int64_t acc = _hpB0 * xq + _hpB1 * _hpX1[c] + _hpB2 * _hpX2[c]
                                      - _hpA1 * _hpY1[c] - _hpA2 * _hpY2[c];
                    const int64_t yq = (acc + (1LL << 29)) >> 30;
                    _hpX2[c] = _hpX1[c]; _hpX1[c] = xq;
                    _hpY2[c] = _hpY1[c]; _hpY1[c] = yq;
                    v = (yq + 2048) >> 12;
                }
                if (v >  32767) v =  32767;
                if (v < -32768) v = -32768;
                if (emitted < kMaxEmit)
                    out[(size_t)emitted * kChannels + c] = (int16_t)v;
            }
            ++emitted;
            _phase += _step;
        }
        _phase -= 65536u;
        return emitted < kMaxEmit ? emitted : kMaxEmit;
    }

    // Decode one delivered row's 64-byte audio field and push it. Layout is four
    // 16-byte blocks, one per microphone, each holding 8 consecutive int16 LE.
    uint32_t PushRowBytes(const uint8_t* audio64, int16_t* out, uint8_t validMask = 0xff)
    {
        int16_t raw[kChannels][8];
        for (uint32_t c = 0; c < kChannels; ++c)
        {
            const uint8_t* blk = audio64 + c * 16;
            for (uint32_t s = 0; s < 8; ++s)
                raw[c][s] = (int16_t)((uint16_t)blk[s * 2] |
                                      ((uint16_t)blk[s * 2 + 1] << 8));
        }
        return PushRow(raw, out, validMask);
    }

    uint32_t Step() const { return _step; }

    // ---- repair the frame-head disturbance --------------------------------
    // The first rows of every frame carry a partial payload -- the row header's
    // valid mask ramps 2,4,4,6,6,6,6 over rows 0-6, identically in 100 % of
    // frames -- and the decoder has no way to tell which slots are real (byte 0
    // does NOT say: honouring it measures WORSE).
    // The FIR spans 8 rows, so that garbage smears across the first ~14 outputs
    // and shows up as a step 2-6x the median at a FIXED offset, 60 times a
    // second, scaling with the signal. That is the roughness on speech that
    // survives the seam repair -- the seam sits at positions 799/0/1/2, so
    // looked four samples to the left of the actual damage.
    //
    // Reconstruction is not available, so conceal instead: the window is fixed,
    // known and about ten samples of eight hundred, and a linear bridge across
    // 0.2 ms is inaudible where a repeating 6x step is not. `count` is the
    // frame's output count, interleaved kChannels.
    static void RepairFrameHead(int16_t* out, uint32_t count,
                                uint32_t lo = 4, uint32_t hi = 14)
    {
        if (count < hi + 1 || lo == 0 || hi <= lo)
            return;
        for (uint32_t c = 0; c < kChannels; ++c)
        {
            const int32_t a = out[(size_t)(lo - 1) * kChannels + c];
            const int32_t b = out[(size_t)hi * kChannels + c];
            const int32_t span = (int32_t)(hi - lo + 1);
            for (uint32_t i = lo; i < hi; ++i)
                out[(size_t)i * kChannels + c] =
                    (int16_t)(a + (b - a) * (int32_t)(i - lo + 1) / span);
        }
    }

private:
    static double I0(double x)          // zeroth-order modified Bessel, for Kaiser
    {
        double s = 1.0, t = 1.0;
        for (int k = 1; k < 40; ++k) { t *= (x / (2.0 * k)) * (x / (2.0 * k)); s += t; }
        return s;
    }

    DspConfig _cfg;
    uint32_t  _taps = 64, _mask = 63;
    int32_t   _fir[kMaxTaps] = {};              // Q15, sums to 32768
    int16_t   _hist[kChannels][kMaxTaps] = {};
    uint32_t  _histPos = 0, _sinceEmit = 0;
    int32_t   _interp[kChannels][4] = {};       // Catmull-Rom window
    uint32_t  _phase = 0;                       // Q16 output phase
    uint32_t  _step  = 1u << 16;                // Q16 inRate/outRate
    int64_t   _hpB0 = 0, _hpB1 = 0, _hpB2 = 0, _hpA1 = 0, _hpA2 = 0;   // Q30
    int64_t   _hpX1[kChannels] = {}, _hpX2[kChannels] = {};            // Q12
    int64_t   _hpY1[kChannels] = {}, _hpY2[kChannels] = {};            // Q12
};

} // namespace ps4mic
