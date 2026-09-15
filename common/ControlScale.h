#pragma once
//
// The mapping between a stored control value and the value a device receives.
//
// Both the transport that writes the device and the UI that labels the slider
// call these. A second copy of the formula anywhere is a label that will
// eventually disagree with the hardware.
//
#include <cstdint>

// Map a stored value in [0, inMax] onto a device value in [0, devMax].
// Truncating, which is what makes the bands uneven: with inMax 255 and devMax 8,
// device value 8 is reachable only at exactly 255.
inline uint32_t ScaleToDevice(uint32_t v, uint32_t inMax, uint32_t devMax)
{
    return inMax ? (uint32_t)((uint64_t)v * devMax / inMax) : 0;
}

// The smallest stored value that yields device value `n`, so that
// ScaleToDevice(StoredForDevice(n, inMax, devMax), inMax, devMax) == n.
inline uint32_t StoredForDevice(uint32_t n, uint32_t inMax, uint32_t devMax)
{
    return devMax ? (uint32_t)(((uint64_t)n * inMax + devMax - 1) / devMax) : 0;
}
