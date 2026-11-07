#pragma once
#include <algorithm>
#include <cstdint>

namespace audiorender {
constexpr double kNominalSourceRate = 48000.0;

// The rate the microphone really delivers, from a measured milli-Hz value.
// Anything outside +/-10 % of nominal (including 0, "not measured yet") is not
// a plausible measurement, so the nominal rate stands in for it.
inline double SourceRate(uint32_t milliHz)
{
    const double rate = milliHz / 1000.0;
    return rate >= 43200.0 && rate <= 52800.0 ? rate : kNominalSourceRate;
}

// The resampler read step the drift loop steers toward: the nominal
// source/sink ratio, nudged in proportion to how far the ring fill is from
// `targetFrames` and clamped to +/-3 % so a bad fill reading cannot run away.
inline double TargetRatio(double sourceRate, uint32_t sinkRate, uint32_t fill,
                          double targetFrames)
{
    const double nominal = sourceRate / sinkRate;
    const double error = (fill - targetFrames) / targetFrames;
    return nominal * (1.0 + std::clamp(0.02 * error, -0.03, 0.03));
}
} // namespace audiorender
