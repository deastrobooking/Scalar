#pragma once
#include "ScalarVoice.h"
#include <cmath>
#include <cstdint>

namespace scalar
{
class DebouncedButton
{
public:
    bool update(bool high, uint32_t nowMs) noexcept
    {
        if (high != candidate) { candidate = high; changedAt = nowMs; }
        if (candidate == stable || uint32_t(nowMs - changedAt) < 20) return false;
        stable = candidate;
        return !stable;
    }
private:
    bool stable = true, candidate = true;
    uint32_t changedAt = 0;
};

struct PitchCalibration
{
    // Calibrated ADC millivolts at 0 V and +1 V input. Nominal ±5 V -> 0..3.0 V.
    float zeroMv = 1500.f, mvPerVolt = 300.f;
    bool convert(float millivolts, float& volts) const noexcept
    {
        if (!std::isfinite(millivolts) || !std::isfinite(zeroMv)
            || !std::isfinite(mvPerVolt) || mvPerVolt <= 0.f) return false;
        volts = juce::jlimit(-5.f, 5.f, (millivolts - zeroMv) / mvPerVolt);
        return true;
    }
};

inline void applyMacros(VoiceParams& p, float timbre, float morph)
{
    timbre = juce::jlimit(0.f, 1.f, timbre);
    morph = juce::jlimit(0.f, 1.f, morph);
    switch (p.engine)
    {
        case Engine::VA: p.va.shapeMorph = timbre * 5.f; p.va.wavefold = morph; break;
        case Engine::Wavetable:
            p.wavetablePosition = timbre;
            p.wavetable.warpMode = WavetableOsc::WarpMode::BendPlus;
            p.wavetable.warpAmount = morph;
            break;
        case Engine::FM: p.fmMorph = morph; p.fmAlgorithm = int(timbre * 39.f); break;
        case Engine::Osc3D: p.osc3d.morphT = morph; p.osc3d.fieldFreq = 0.25f + timbre * 7.75f; break;
        case Engine::Terrain: p.terrain.morph = morph; p.terrain.scanRate = 0.05f + timbre * 4.f; break;
        case Engine::Granular: p.granular.position = timbre; p.granular.sizeMs = 5.f + morph * 495.f; break;
        case Engine::NumEngines: break;
    }
}
}
