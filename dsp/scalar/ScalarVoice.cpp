#include "scalar/ScalarVoice.h"

namespace scalar
{

const char* engineName(Engine e) noexcept
{
    switch (e)
    {
        case Engine::VA:        return "VA";
        case Engine::Wavetable: return "WT";
        case Engine::FM:        return "FM";
        case Engine::Osc3D:     return "3D";
        case Engine::Terrain:   return "TERRAIN";
        case Engine::Granular:  return "GRAN";
        case Engine::NumEngines: break;
    }
    return "?";
}

void ScalarVoice::prepare(double sampleRate)
{
    sr = sampleRate;

    osc3dEngine.prepare(sr);
    terrainEngine.prepare(sr);
    wavetableEngine.prepare(sr);
    fmEngine.prepare(sr);
    vaEngine.prepare(sr);
    granularEngine.prepare(sr);

    filter.prepare(sr);
    ampEnv.prepare(sr);

    setParams(params);
    setFrequency(currentHz);
}

void ScalarVoice::reset()
{
    osc3dEngine.reset();
    terrainEngine.reset();
    wavetableEngine.reset();
    fmEngine.reset();
    vaEngine.reset();
    granularEngine.reset();
    filter.reset();
    ampEnv.reset();
}

void ScalarVoice::setParams(const VoiceParams& p)
{
    const bool engineChanged = p.engine != activeEngine;
    params = p;
    activeEngine = p.engine;

    osc3dEngine.setParams(p.osc3d);
    terrainEngine.setParams(p.terrain);
    wavetableEngine.setParams(p.wavetable);
    vaEngine.setParams(p.va);
    granularEngine.setParams(p.granular);
    fmEngine.setAlgorithm(p.fmAlgorithm);
    fmEngine.setAlgorithmMorph(p.fmMorph);

    filter.setFilterA(p.filterA);
    filter.setFilterB(p.filterB);
    filter.setRouting(p.routing);
    filter.getEnvA().setParams(p.filterEnv);
    filter.getEnvB().setParams(p.filterEnv);

    ampEnv.setParams(p.ampEnv);

    if (engineChanged)
        reset();
}

void ScalarVoice::setFrequency(float hz)
{
    currentHz = hz;
    osc3dEngine.setFrequency(hz);
    terrainEngine.setFrequency(hz);
    wavetableEngine.setFrequency(hz);
    fmEngine.setNoteFrequency(hz);
    vaEngine.setFrequency(hz);
    granularEngine.setFrequency(hz);
}

void ScalarVoice::noteOn(int midiNote, float velocity)
{
    currentNote = midiNote;
    currentVelocity = velocity;

    const float hz = 440.f * std::exp2((static_cast<float>(midiNote) - 69.f) / 12.f);
    setFrequency(hz);

    fmEngine.setMidiNote(midiNote);
    fmEngine.setVelocity(velocity);

    switch (activeEngine)
    {
        case Engine::Wavetable: wavetableEngine.noteOn(); break;
        case Engine::FM:        fmEngine.noteOn(); break;
        case Engine::Granular:  granularEngine.noteOn(midiNote, velocity); break;
        default: break;
    }

    filter.noteOn();
    ampEnv.noteOn();
}

void ScalarVoice::noteOff()
{
    if (activeEngine == Engine::FM)
        fmEngine.noteOff();
    if (activeEngine == Engine::Granular)
        granularEngine.noteOff();

    filter.noteOff();
    ampEnv.noteOff();
}

std::pair<float, float> ScalarVoice::renderEngine()
{
    switch (activeEngine)
    {
        case Engine::VA:
            return vaEngine.processSample();

        case Engine::Wavetable:
            return wavetableEngine.processSample();

        case Engine::FM:
        {
            const float s = fmEngine.processSample();
            fmEngine.advanceEnvelopes();
            return { s, s };
        }

        case Engine::Osc3D:
        {
            const float s = osc3dEngine.processSample();
            return { s, s };
        }

        case Engine::Terrain:
        {
            const float s = terrainEngine.processSample();
            return { s, s };
        }

        case Engine::Granular:
            return granularEngine.processSample();

        case Engine::NumEngines:
            break;
    }
    return { 0.f, 0.f };
}

std::pair<float, float> ScalarVoice::processSample()
{
    auto [l, r] = renderEngine();

    auto [fl, fr] = filter.processSample(l, r, 0.f, 0.f);

    const float amp = ampEnv.processSample() * params.level * currentVelocity;
    return { fl * amp, fr * amp };
}

} // namespace scalar
