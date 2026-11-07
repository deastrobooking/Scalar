// scalar_render — native development harness for the Scalar voice.
//
// Renders a short phrase through each engine to 48 kHz stereo WAV files so the
// DSP core can be heard and validated without ESP32 hardware.
//
//   scalar_render [outDir]     render one WAV per engine into outDir (default .)
//   scalar_render --smoke      render all engines to memory, verify finite,
//                              non-silent output; exit non-zero on failure

#include "scalar/ScalarVoice.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

namespace
{
constexpr double kSampleRate = 48000.0;
constexpr int kNoteSamples   = 48000;      // 1 s held note
constexpr int kTailSamples   = 24000;      // 0.5 s release tail

void writeWav(const std::string& path, const std::vector<float>& interleaved)
{
    const uint32_t numFrames = static_cast<uint32_t>(interleaved.size() / 2);
    const uint32_t dataBytes = numFrames * 2 * 2;

    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f)
    {
        std::fprintf(stderr, "cannot open %s\n", path.c_str());
        return;
    }

    auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
    auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

    std::fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); std::fwrite("WAVE", 1, 4, f);
    std::fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(2);
    u32(static_cast<uint32_t>(kSampleRate));
    u32(static_cast<uint32_t>(kSampleRate) * 4); u16(4); u16(16);
    std::fwrite("data", 1, 4, f); u32(dataBytes);

    for (float s : interleaved)
    {
        const float c = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
        const int16_t v = static_cast<int16_t>(c * 32767.f);
        std::fwrite(&v, 2, 1, f);
    }
    std::fclose(f);
}

struct RenderStats
{
    float peak = 0.f;
    bool finite = true;
};

RenderStats renderEngine(scalar::Engine engine, std::vector<float>& out)
{
    scalar::ScalarVoice voice;

    scalar::VoiceParams p;
    p.engine = engine;
    p.ampEnv.attackSec = 0.005f;
    p.ampEnv.releaseSec = 0.25f;
    p.filterA.mode = FilterMode::LP24;
    p.filterA.cutoff = 9000.f;
    p.filterA.resonance = 0.25f;

    // Give each engine a little character so the renders are distinguishable.
    switch (engine)
    {
        case scalar::Engine::VA:
            p.va.shape = VAOscillator::Shape::Saw;
            p.va.unisonCount = 3;
            p.va.detune = 0.08f;
            break;
        case scalar::Engine::Wavetable:
            p.wavetable.unison = 2;
            p.wavetable.detune = 0.05f;
            break;
        case scalar::Engine::Osc3D:
            p.osc3d.field = Osc3D::FieldType::HarmonicLattice;
            p.osc3d.path  = Osc3D::PathType::Lissajous;
            break;
        case scalar::Engine::Terrain:
            p.terrain.scanRate = 1.f;
            break;
        case scalar::Engine::Granular:
            p.granular.density = 40.f;
            p.granular.sizeMs = 90.f;
            break;
        default:
            break;
    }

    voice.prepare(kSampleRate);
    voice.setParams(p);

    if (engine == scalar::Engine::FM)
    {
        // Simple 2-operator bright FM patch on top of the default algorithm.
        auto& carrier = voice.fm().getOperator(0);
        auto& mod     = voice.fm().getOperator(1);
        FMOperator::Params cp, mp;
        cp.coarse = 1; cp.level = 1.f;
        mp.coarse = 2; mp.level = 0.55f;
        carrier.setParams(cp);
        mod.setParams(mp);
    }

    voice.noteOn(57, 0.9f); // A3

    RenderStats stats;
    out.clear();
    out.reserve(static_cast<size_t>(kNoteSamples + kTailSamples) * 2);

    for (int i = 0; i < kNoteSamples + kTailSamples; ++i)
    {
        if (i == kNoteSamples)
            voice.noteOff();

        auto [l, r] = voice.processSample();
        if (!std::isfinite(l) || !std::isfinite(r))
        {
            stats.finite = false;
            l = r = 0.f;
        }
        stats.peak = std::fmax(stats.peak, std::fmax(std::fabs(l), std::fabs(r)));
        out.push_back(l);
        out.push_back(r);
    }
    return stats;
}
} // namespace

int main(int argc, char** argv)
{
    const bool smoke = argc > 1 && std::strcmp(argv[1], "--smoke") == 0;
    const std::string outDir = (!smoke && argc > 1) ? argv[1] : ".";

    bool ok = true;
    std::vector<float> buffer;

    for (int e = 0; e < static_cast<int>(scalar::Engine::NumEngines); ++e)
    {
        const auto engine = static_cast<scalar::Engine>(e);
        const auto stats = renderEngine(engine, buffer);

        const char* name = scalar::engineName(engine);
        std::printf("%-8s peak=%.3f finite=%s\n", name, stats.peak,
                    stats.finite ? "yes" : "NO");

        if (!stats.finite || stats.peak < 1.0e-4f)
            ok = false;

        if (!smoke)
            writeWav(outDir + "/scalar_" + name + ".wav", buffer);
    }

    if (!ok)
    {
        std::fprintf(stderr, "FAIL: an engine produced silent or non-finite output\n");
        return 1;
    }
    std::printf("all engines OK\n");
    return 0;
}
