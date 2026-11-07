#pragma once
// ScalarVoice — one full synth voice for the Scalar Eurorack module.
//
// Plaits-style design: six GLIESE 710 engines are compiled in, one is active
// at a time. Signal flow:  engine -> dual filter (with filter envelopes)
// -> amp ADSR/VCA -> stereo out.

#include "gliese/Oscillators/Osc3D.h"
#include "gliese/Oscillators/TerrainOsc.h"
#include "gliese/Oscillators/WavetableOsc.h"
#include "gliese/Oscillators/FMAlgorithm.h"
#include "gliese/Oscillators/VAOscillator.h"
#include "gliese/Oscillators/GranularOsc.h"
#include "gliese/Envelopes/ADSREnvelope.h"
#include "gliese/Filter/DualFilter.h"

#include <utility>

namespace scalar
{

enum class Engine
{
    VA = 0,
    Wavetable,
    FM,
    Osc3D,
    Terrain,
    Granular,
    NumEngines
};

const char* engineName(Engine e) noexcept;

struct VoiceParams
{
    Engine engine = Engine::VA;

    Osc3D::Params        osc3d {};
    TerrainOsc::Params   terrain {};
    WavetableOsc::Params wavetable {};
    VAOscillator::Params va {};
    GranularOsc::Params  granular {};

    int   fmAlgorithm = 0;
    float fmMorph     = 0.f;

    SingleFilter::Params filterA {};
    SingleFilter::Params filterB {};
    FilterRouting        routing = FilterRouting::AOnly;
    ADSREnvelope::Params filterEnv {};

    ADSREnvelope::Params ampEnv {};
    float level = 0.8f;
};

class ScalarVoice
{
public:
    void prepare(double sampleRate);
    void reset();

    // Control-rate updates; cheap when values are unchanged.
    void setParams(const VoiceParams& p);

    // Continuous pitch (V/Oct). Safe to call every control tick.
    void setFrequency(float hz);

    void noteOn(int midiNote, float velocity);
    void noteOff();
    bool isActive() const noexcept { return ampEnv.isActive(); }

    // Direct access for deep FM programming from the control layer.
    FMAlgorithm& fm() noexcept { return fmEngine; }
    GranularOsc& granular() noexcept { return granularEngine; }
    TerrainOsc&  terrain() noexcept { return terrainEngine; }
    WavetableOsc& wavetable() noexcept { return wavetableEngine; }

    std::pair<float, float> processSample();

private:
    double sr = 48000.0;
    VoiceParams params;
    Engine activeEngine = Engine::VA;

    Osc3D        osc3dEngine;
    TerrainOsc   terrainEngine;
    WavetableOsc wavetableEngine;
    FMAlgorithm  fmEngine;
    VAOscillator vaEngine;
    GranularOsc  granularEngine;

    DualFilter   filter;
    ADSREnvelope ampEnv;

    float currentHz = 440.f;
    int   currentNote = 69;
    float currentVelocity = 1.f;

    std::pair<float, float> renderEngine();
};

} // namespace scalar
