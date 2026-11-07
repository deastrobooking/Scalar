#pragma once
#include <bit>
#include <cstdint>

namespace scalar
{
// IEEE-754 bit test remains valid even in a caller compiled with fast math.
inline bool finiteAudio(float value) noexcept
{
    return (std::bit_cast<uint32_t>(value) & 0x7f800000u) != 0x7f800000u;
}
inline int16_t toPcm(float sample) noexcept
{
    if (!finiteAudio(sample)) return 0;
    sample = sample < -1.f ? -1.f : (sample > 1.f ? 1.f : sample);
    return static_cast<int16_t>(sample * 32767.f);
}
}
