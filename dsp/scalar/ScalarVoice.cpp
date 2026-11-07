#include "scalar/ScalarVoice.h"
#include "scalar/AudioUtils.h"

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

void ScalarVoice::prepare(double sampleRate, void (*yieldDuringPrepare)())
{
    sr = std::isfinite(sampleRate) && sampleRate > 0.0 ? juce::jmin(384000.0, sampleRate) : 48000.0;

    osc3dEngine.prepare(sr);
    terrainEngine.prepare(sr);
    if (yieldDuringPrepare) yieldDuringPrepare();
    wavetableEngine.prepare(sr, yieldDuringPrepare);
    fmEngine.prepare(sr);
    vaEngine.prepare(sr);
    granularEngine.prepare(sr, yieldDuringPrepare);

    filter.prepare(sr);
    ampEnv.prepare(sr);

    setParams(params);
    setFrequency(currentHz);
    reset();
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
    gateHeld = false;
    transitionRemaining = 0;
    lastOutput = transitionFrom = {0.f, 0.f};
}

void ScalarVoice::setParams(const VoiceParams& p)
{
    const Engine nextEngine = static_cast<Engine>(juce::jlimit(0, int(Engine::NumEngines) - 1, int(p.engine)));
    const bool engineChanged = nextEngine != activeEngine;
    if (engineChanged)
    {
        if (activeEngine == Engine::FM) fmEngine.noteOff();
        if (activeEngine == Engine::Granular) granularEngine.noteOff();
        transitionFrom = lastOutput;
        transitionRemaining = kTransitionSamples;
    }
    params = p;
    activeEngine = nextEngine;
    params.level = finiteAudio(p.level) ? juce::jlimit(0.f, 1.f, p.level) : 0.f;
    switch (activeEngine)
    {
        case Engine::VA: vaEngine.setParams(p.va); break;
        case Engine::Wavetable:
        {
            wavetableEngine.setParams(p.wavetable);
            const float position = juce::jlimit(0.f, 1.f, p.wavetablePosition)
                * juce::jmax(0, wavetableEngine.getNumTables() - 1);
            const int index = int(position);
            wavetableEngine.setTablePosition(index, position - index);
            break;
        }
        case Engine::FM:
            fmEngine.setAlgorithm(p.fmAlgorithm);
            fmEngine.setAlgorithmMorph(p.fmMorph);
            break;
        case Engine::Osc3D: osc3dEngine.setParams(p.osc3d); break;
        case Engine::Terrain: terrainEngine.setParams(p.terrain); break;
        case Engine::Granular: granularEngine.setParams(p.granular); break;
        case Engine::NumEngines: break;
    }

    filter.setFilterA(p.filterA);
    filter.setFilterB(p.filterB);
    filter.setRouting(p.routing);
    filter.getEnvA().setParams(p.filterEnv);
    filter.getEnvB().setParams(p.filterEnv);

    ampEnv.setParams(p.ampEnv);

    if (engineChanged)
    {
        resetEngine();
        setFrequency(currentHz);
        if (ampEnv.isActive())
        {
            startEngine();
            if (!gateHeld)
            {
                if (activeEngine == Engine::FM) fmEngine.noteOff();
                if (activeEngine == Engine::Granular) granularEngine.noteOff();
            }
        }
    }
}

void ScalarVoice::setFrequency(float hz)
{
    if (!finiteAudio(hz) || hz <= 0.f) return;
    currentHz = juce::jlimit(1.f, juce::jmin(20000.f, float(sr * 0.45)), hz);
    switch (activeEngine)
    {
        case Engine::VA: vaEngine.setFrequency(currentHz); break;
        case Engine::Wavetable: wavetableEngine.setFrequency(currentHz); break;
        case Engine::FM: fmEngine.setNoteFrequency(currentHz); break;
        case Engine::Osc3D: osc3dEngine.setFrequency(currentHz); break;
        case Engine::Terrain: terrainEngine.setFrequency(currentHz); break;
        case Engine::Granular: granularEngine.setFrequency(currentHz); break;
        case Engine::NumEngines: break;
    }
}

void ScalarVoice::noteOn(int midiNote, float velocity)
{
    const float hz = 440.f * std::exp2((static_cast<float>(midiNote) - 69.f) / 12.f);
    noteOn(midiNote, velocity, hz);
}

void ScalarVoice::noteOn(int midiNote, float velocity, float hz)
{
    currentNote = juce::jlimit(0, 127, midiNote);
    currentVelocity = finiteAudio(velocity) ? juce::jlimit(0.f, 1.f, velocity) : 0.f;
    gateHeld = true;
    setFrequency(hz);
    startEngine();
    filter.noteOn();
    ampEnv.noteOn();
}

void ScalarVoice::startEngine()
{
    switch (activeEngine)
    {
        case Engine::Wavetable: wavetableEngine.noteOn(); break;
        case Engine::FM:
            fmEngine.setMidiNote(currentNote);
            fmEngine.setVelocity(currentVelocity);
            fmEngine.noteOn();
            break;
        case Engine::Granular: granularEngine.noteOn(currentNote, currentVelocity); break;
        default: break;
    }
}

void ScalarVoice::resetEngine()
{
    switch (activeEngine)
    {
        case Engine::VA: vaEngine.reset(); break;
        case Engine::Wavetable: wavetableEngine.reset(); break;
        case Engine::FM: fmEngine.reset(); break;
        case Engine::Osc3D: osc3dEngine.reset(); break;
        case Engine::Terrain: terrainEngine.reset(); break;
        case Engine::Granular: granularEngine.reset(); break;
        case Engine::NumEngines: break;
    }
}

void ScalarVoice::noteOff()
{
    gateHeld = false;
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
    if (!finiteAudio(l) || !finiteAudio(r))
    {
        resetEngine();
        filter.reset();
        if (gateHeld) startEngine();
        l = r = 0.f;
    }

    auto [fl, fr] = filter.processSample(l, r, 0.f, 0.f);

    const float amp = ampEnv.processSample() * params.level * currentVelocity;
    std::pair<float, float> out { fl * amp, fr * amp };
    if (!finiteAudio(out.first) || !finiteAudio(out.second))
    {
        filter.reset();
        out = {0.f, 0.f};
    }
    if (transitionRemaining > 0)
    {
        const float t = float(kTransitionSamples - transitionRemaining + 1) / kTransitionSamples;
        out.first = transitionFrom.first + t * (out.first - transitionFrom.first);
        out.second = transitionFrom.second + t * (out.second - transitionFrom.second);
        --transitionRemaining;
    }
    return lastOutput = out;
}

} // namespace scalar
