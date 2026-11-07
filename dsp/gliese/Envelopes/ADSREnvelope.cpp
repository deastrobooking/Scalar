#include "ADSREnvelope.h"

namespace
{
constexpr float kAttackRatio = 0.3f;
constexpr float kDecayReleaseRatio = 0.0001f;
float validTime(float seconds)
{
    return std::isfinite(seconds) ? juce::jlimit(0.f, 60.f, seconds) : 0.f;
}
}

float ADSREnvelope::calcCoeff(int samples, float ratio)
{
    return std::exp(-std::log((1.f + ratio) / ratio) / static_cast<float>(samples));
}

void ADSREnvelope::recalcCoeffs()
{
    attackSamples = juce::jmax(1, int(std::lround(params.attackSec * sr)));
    decaySamples = juce::jmax(1, int(std::lround(params.decaySec * sr)));
    releaseSamples = juce::jmax(1, int(std::lround(params.releaseSec * sr)));
    attackCoeff = calcCoeff(attackSamples, kAttackRatio);
    decayCoeff = calcCoeff(decaySamples, kDecayReleaseRatio);
    releaseCoeff = calcCoeff(releaseSamples, kDecayReleaseRatio);
}

void ADSREnvelope::prepare(double sampleRate)
{
    sr = std::isfinite(sampleRate) && sampleRate > 0.0 ? juce::jmin(384000.0, sampleRate) : 48000.0;
    recalcCoeffs();
    reset();
}

void ADSREnvelope::setParams(const Params& p)
{
    Params next = p;
    next.attackSec = validTime(next.attackSec);
    next.decaySec = validTime(next.decaySec);
    next.releaseSec = validTime(next.releaseSec);
    next.sustain = std::isfinite(next.sustain) ? juce::jlimit(0.f, 1.f, next.sustain) : 0.f;
    if (next.attackSec == params.attackSec && next.decaySec == params.decaySec
        && next.releaseSec == params.releaseSec && next.sustain == params.sustain && next.curve == params.curve)
        return;
    params = next;
    recalcCoeffs();
    if (stage == Stage::Attack || stage == Stage::Decay || stage == Stage::Release)
        enterStage(stage);
}

void ADSREnvelope::enterStage(Stage next)
{
    stage = next;
    switch (stage)
    {
        case Stage::Attack:
            remaining = attackSamples; destination = 1.f; coefficient = attackCoeff;
            target = 1.f + kAttackRatio * (1.f - value);
            break;
        case Stage::Decay:
            remaining = decaySamples; destination = params.sustain; coefficient = decayCoeff;
            target = destination - kDecayReleaseRatio * (value - destination);
            break;
        case Stage::Release:
            remaining = releaseSamples; destination = 0.f; coefficient = releaseCoeff;
            target = -kDecayReleaseRatio * value;
            break;
        default: remaining = 0; return;
    }
    linearStep = (destination - value) / static_cast<float>(remaining);
}

void ADSREnvelope::noteOn() { enterStage(Stage::Attack); }
void ADSREnvelope::noteOff() { if (stage != Stage::Idle) enterStage(Stage::Release); }
void ADSREnvelope::reset() { stage = Stage::Idle; value = 0.f; remaining = 0; }

float ADSREnvelope::processSample()
{
    if (stage == Stage::Idle) return 0.f;
    if (stage == Stage::Sustain) return value = params.sustain;
    if (params.curve == Curve::Linear)
        value += linearStep;
    else
        value = target + coefficient * (value - target);
    value = juce::jlimit(0.f, 1.f, value);
    // Explicit stage lengths avoid accumulated single-precision error changing timing.
    if (--remaining == 0)
    {
        value = destination;
        if (stage == Stage::Attack)
        {
            if (params.sustain == 1.f) stage = Stage::Sustain;
            else enterStage(Stage::Decay);
        }
        else if (stage == Stage::Decay) stage = Stage::Sustain;
        else stage = Stage::Idle;
    }
    return value;
}
