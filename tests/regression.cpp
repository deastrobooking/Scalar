#include "scalar/ScalarVoice.h"
#include "scalar/ControlRouter.h"
#include "scalar/ControlUtils.h"
#include "scalar/AudioUtils.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <thread>
#include <vector>

namespace
{
int preparationYields = 0;
void yieldDuringPrepare() { ++preparationYields; }
void check(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::abort(); }
}
bool close(float a, float b, float tolerance = 1.e-4f) { return std::abs(a - b) < tolerance; }
std::vector<scalar::VoiceEvent> drain(scalar::ControlTransport& transport)
{
    std::vector<scalar::VoiceEvent> events;
    scalar::VoiceEvent event;
    while (transport.receive(event)) events.push_back(event);
    return events;
}
void packet(scalar::ControlRouter& router, uint8_t status, uint8_t a, uint8_t b)
{
    const uint8_t data[] = {uint8_t(status >> 4), status, a, b};
    router.midi(data);
}

void transportTests()
{
    scalar::ControlTransport transport;
    scalar::VoiceParams out;
    check(!transport.consume(out), "unpublished parameters are unavailable");
    transport.staging().level = .1f; transport.publish();
    check(transport.consume(out) && close(out.level, .1f), "first snapshot");
    transport.staging().level = .2f; transport.publish();
    transport.staging().level = .3f; transport.publish();
    check(transport.consume(out) && close(out.level, .3f), "two publications cannot hide the latest snapshot");
    check(!transport.consume(out), "snapshot is consumed once");

    struct Frame { uint32_t sequence = 0; std::array<uint32_t, 256> words {}; };
    scalar::LatestSnapshot<Frame> snapshots;
    constexpr uint32_t count = 100000;
    std::atomic<bool> done {false};
    std::thread producer([&] {
        for (uint32_t i = 1; i <= count; ++i)
        {
            snapshots.staging().sequence = i;
            snapshots.staging().words.fill(i);
            snapshots.publish();
        }
        done.store(true, std::memory_order_release);
    });
    Frame frame;
    uint32_t last = 0;
    auto consume = [&] {
        if (!snapshots.consume(frame)) return;
        check(frame.sequence > last, "snapshot sequence is monotonic");
        for (auto word : frame.words) check(word == frame.sequence, "snapshot cannot tear during concurrent publication");
        last = frame.sequence;
    };
    while (!done.load(std::memory_order_acquire)) consume();
    producer.join(); consume();
    check(last == count, "final snapshot is visible");

    scalar::SpscQueue<uint32_t, 31> queue;
    std::thread eventProducer([&] {
        for (uint32_t i = 0; i < count; ++i) while (!queue.push(i)) std::this_thread::yield();
    });
    for (uint32_t i = 0; i < count; ++i)
    {
        uint32_t value;
        while (!queue.pop(value)) std::this_thread::yield();
        check(value == i, "event order survives queue wrap and producer/consumer contention");
    }
    eventProducer.join();

    for (size_t i = 0; i < transport.kEventCapacity; ++i)
        check(transport.send({scalar::VoiceEventType::NoteOn}), "queue accepts its documented capacity");
    check(!transport.send({}), "overflow is detected");
    check(!transport.send({}), "producer stops until audio acknowledges overflow");
    check(transport.silenceAndFlushRequested(), "audio receives panic request");
    check(transport.recoveryReady() && drain(transport).empty(), "panic flushes old notes before recovery");
    check(!transport.send({}), "producer remains stopped until owner resets musical state");
    transport.acknowledgeRecovery();
    check(transport.send({scalar::VoiceEventType::NoteOff}), "queue recovers without stale events");
    check(transport.overflowCount() == 1, "overflow telemetry counts failures");
    drain(transport);
    transport.send({scalar::VoiceEventType::NoteOn, 432.f, 1.f, 69, 1000});
    transport.send({scalar::VoiceEventType::NoteOff, 432.f, 1.f, 69, 2000});
    scalar::VoiceEvent event;
    check(transport.receiveAt(event, 100, 48000), "first captured edge establishes event clock");
    check(!transport.receiveAt(event, 147, 48000), "queued pulse stays high for its captured duration");
    check(transport.receiveAt(event, 148, 48000) && event.type == scalar::VoiceEventType::NoteOff,
        "one millisecond pulse lasts 48 render frames");
}

void routerTests()
{
    scalar::ControlTransport transport;
    scalar::ControlRouter router(transport);
    router.setCvPitch(432.f); router.setCvGate(true); drain(transport);
    packet(router, 0x90, 69, 64);
    auto events = drain(transport);
    check(events.size() == 1 && events[0].type == scalar::VoiceEventType::NoteOn
        && close(events[0].frequency, 440.f) && close(events[0].velocity, 64.f / 127), "MIDI note carries coherent pitch and velocity");
    router.setCvPitch(300.f); router.setCvGate(false); router.setCvGate(true);
    check(drain(transport).empty(), "CV cannot overwrite held MIDI");
    packet(router, 0xe0, 0, 96);
    events = drain(transport);
    check(events.size() == 1 && close(events[0].frequency, 440.f * std::exp2(1.f / 12.f)), "pitch bend retains continuous frequency");
    packet(router, 0x90, 72, 100); drain(transport);
    packet(router, 0x80, 72, 0);
    events = drain(transport);
    check(events.size() == 1 && events[0].note == 69, "last-note priority returns to a previously held note");
    packet(router, 0x90, 69, 0);
    events = drain(transport);
    check(events.size() == 2 && events.back().type == scalar::VoiceEventType::NoteOn
        && close(events.back().frequency, 300.f) && events.back().velocity == 1.f, "MIDI release restores CV with full velocity");
    packet(router, 0x91, 60, 127);
    check(drain(transport).empty(), "other MIDI channels are ignored");
    packet(router, 0x90, 60, 127); packet(router, 0x80, 60, 0); packet(router, 0x90, 60, 32);
    events = drain(transport);
    check(events.size() == 4 && events.back().type == scalar::VoiceEventType::NoteOn
        && close(events.back().velocity, 32.f / 127), "rapid off/on preserves the next note and its velocity");
    packet(router, 0xb0, 123, 0);
    check(drain(transport).back().frequency == 300.f, "all-notes-off resumes CV");
    packet(router, 0x90, 60, 127); drain(transport);
    router.disconnect();
    check(drain(transport).back().velocity == 1.f, "disconnect clears held notes");
    router.setCvGate(false); drain(transport);
    router.setCvGate(true); router.setCvGate(false); router.setCvGate(true); router.setCvGate(false);
    events = drain(transport);
    check(events.size() == 4 && events[0].type == scalar::VoiceEventType::NoteOn
        && events[1].type == scalar::VoiceEventType::NoteOff, "short captured gate pulses survive until rendering");
}

void envelopeTests()
{
    for (double sr : {44100., 48000., 96000.})
        for (auto curve : {ADSREnvelope::Curve::Linear, ADSREnvelope::Curve::Exponential})
        {
            ADSREnvelope env;
            ADSREnvelope::Params p;
            p.attackSec = .1f; p.decaySec = .2f; p.sustain = .4f; p.releaseSec = .3f; p.curve = curve;
            env.prepare(sr); env.setParams(p); env.noteOn();
            const int attack = int(std::lround(p.attackSec * sr));
            for (int i = 0; i < attack - 1; ++i) { env.processSample(); if (i % 100 == 0) env.setParams(p); }
            check(env.getStage() == ADSREnvelope::Stage::Attack, "attack lasts its selected duration despite repeated parameters");
            env.processSample();
            check(env.getStage() == ADSREnvelope::Stage::Decay && env.getCurrentValue() == 1.f, "attack completes at the correct sample");
            const int decay = int(std::lround(p.decaySec * sr));
            for (int i = 0; i < decay; ++i) env.processSample();
            check(env.getStage() == ADSREnvelope::Stage::Sustain && env.getCurrentValue() == p.sustain, "decay timing and sustain endpoint");
            env.noteOff();
            const int release = int(std::lround(p.releaseSec * sr));
            for (int i = 0; i < release - 1; ++i) env.processSample();
            check(env.isActive(), "release lasts its selected duration");
            env.processSample(); check(!env.isActive() && env.getCurrentValue() == 0.f, "release ends cleanly");
        }
    ADSREnvelope env; ADSREnvelope::Params p;
    p.attackSec = 1.f; p.curve = ADSREnvelope::Curve::Linear;
    env.prepare(48000); env.setParams(p); env.noteOn();
    for (int i = 0; i < 24000; ++i) env.processSample();
    check(close(env.getCurrentValue(), .5f, .001f), "linear curve is implemented");
    p.attackSec = p.decaySec = p.releaseSec = 0.f;
    env.setParams(p); env.noteOn(); env.processSample(); env.processSample(); env.noteOff(); env.processSample();
    check(!env.isActive(), "zero-time stages complete without division by zero");
}

float renderPeak(scalar::ScalarVoice& voice, int samples)
{
    float peak = 0.f;
    for (int i = 0; i < samples; ++i)
    {
        auto [l, r] = voice.processSample();
        check(scalar::finiteAudio(l) && scalar::finiteAudio(r), "voice output stays finite");
        peak = std::max({peak, std::abs(l), std::abs(r)});
    }
    return peak;
}

void voiceTests()
{
    scalar::ScalarVoice voice; scalar::VoiceParams p;
    p.ampEnv.attackSec = p.ampEnv.decaySec = 0.f; p.ampEnv.sustain = 1.f;
    p.filterA.mix = 0.f; p.va.shape = VAOscillator::Shape::Sine;
    voice.prepare(48000, yieldDuringPrepare);
    check(preparationYields > 0, "heavy startup preparation cooperatively yields");
    const int startupYields = preparationYields;
    voice.setParams(p); voice.noteOn(69, 1.f, 432.f);
    renderPeak(voice, 4800);
    int crossings = 0; float previous = 0.f;
    for (int i = 0; i < 48000; ++i)
    {
        float next = voice.processSample().first;
        if (previous < 0.f && next >= 0.f) ++crossings;
        previous = next;
    }
    check(crossings == 432, "exact note-on pitch remains 432 Hz");
    voice.noteOn(24, 1.f, 4.1f); renderPeak(voice, 48000);
    crossings = 0; previous = 0;
    for (int i = 0; i < 48000; ++i)
    {
        float next = voice.processSample().first;
        if (previous < 0 && next >= 0) ++crossings;
        previous = next;
    }
    check(crossings >= 4 && crossings <= 5, "low CV pitch is not clamped to 20 Hz");
    voice.noteOn(69, 1.f, 432.f);
    for (int engine = 1; engine < int(scalar::Engine::NumEngines); ++engine)
    {
        const auto before = voice.processSample();
        p.engine = scalar::Engine(engine); voice.setParams(p);
        const auto after = voice.processSample();
        check(std::abs(after.first - before.first) < .03f && std::abs(after.second - before.second) < .03f,
            "engine transition begins continuously without rendering two full engines");
        check(voice.isActive(), "engine change preserves held envelope");
        check(renderPeak(voice, 9600) > .001f, "held note remains audible after engine change");
    }
    p.engine = scalar::Engine::FM; voice.setParams(p);
    ADSREnvelope reference;
    ADSREnvelope::Params ep; ep.attackSec = .1f; ep.sustain = 1.f;
    reference.prepare(48000); reference.setParams(ep); reference.noteOn();
    voice.fm().getOperator(0).getEnvelope().setParams(ep);
    voice.reset(); voice.noteOn(69, 1.f, 432.f);
    for (int i = 0; i < 2400; ++i) { reference.processSample(); voice.processSample(); }
    check(close(voice.fm().getOperator(0).getEnvValue(), reference.getCurrentValue()), "FM envelope advances once per audio sample");
    voice.noteOff(); renderPeak(voice, 24000);
    check(!voice.isActive(), "voice releases after switching engines");
    check(preparationYields == startupYields, "rendering never invokes the startup yield hook");
}

void macroAndInputTests()
{
    scalar::VoiceParams p; p.engine = scalar::Engine::Wavetable;
    scalar::applyMacros(p, .8f, .7f);
    check(close(p.wavetablePosition, .8f) && p.wavetable.warpMode != WavetableOsc::WarpMode::Off, "WT panel macros select scan and an active warp");
    scalar::ScalarVoice voice; voice.prepare(48000);
    p.filterA.mix = 0; p.ampEnv.attackSec = p.ampEnv.decaySec = 0; p.ampEnv.sustain = 1;
    scalar::applyMacros(p, 0, 0); voice.setParams(p); voice.noteOn(69, 1);
    renderPeak(voice, 512);
    std::array<float, 2048> reference;
    for (auto& x : reference) x = voice.processSample().first;
    scalar::applyMacros(p, 0, 1); voice.setParams(p); voice.noteOn(69, 1); renderPeak(voice, 512);
    float difference = 0;
    for (float x : reference) difference += std::abs(x - voice.processSample().first);
    check(difference > 10.f, "WT morph audibly changes the stock patch");
    scalar::applyMacros(p, .5f, 0); voice.setParams(p); voice.noteOn(69, 1); renderPeak(voice, 512);
    difference = 0;
    for (float x : reference) difference += std::abs(x - voice.processSample().first);
    check(difference > 10.f, "WT table scan changes the stock patch");

    scalar::DebouncedButton button;
    check(!button.update(false, 10), "button waits for stable press");
    check(!button.update(true, 12) && !button.update(false, 14), "bounce is ignored");
    check(!button.update(false, 33) && button.update(false, 34), "one stable falling edge");
    check(!button.update(false, 100), "held button does not repeat");
    button.update(true, 101); button.update(true, 121); button.update(false, 122);
    check(button.update(false, 142), "second debounced press");
    scalar::PitchCalibration calibration {1510, 298}; float volts = 0;
    check(calibration.convert(1510, volts) && volts == 0, "pitch calibration removes offset");
    check(calibration.convert(1808, volts) && close(volts, 1), "pitch calibration removes gain error");
    check(!calibration.convert(std::numeric_limits<float>::quiet_NaN(), volts), "invalid ADC voltage is rejected");
    calibration.mvPerVolt = 0;
    check(!calibration.convert(1510, volts), "invalid calibration is rejected");
}

void memoryAndSafetyTests()
{
    TerrainOsc terrain; terrain.prepare(48000);
    check(!terrain.setHeightmap(nullptr, 128, 128), "null terrain rejected");
    float dummy = 0;
    check(!terrain.setHeightmap(&dummy, 512, 512), "oversized terrain is rejected before reading or allocating");
    check(terrain.getWidth() == 128 && terrain.getHeight() == 128, "rejected terrain preserves existing asset");
    std::vector<float> grid(128 * 128, .3f);
    check(terrain.setHeightmap(grid.data(), 128, 128), "maximum budgeted terrain is accepted");
    for (int i = 0; i < 512; ++i) check(scalar::finiteAudio(terrain.processSample()), "budgeted terrain renders");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    check(scalar::toPcm(nan) == 0 && scalar::toPcm(inf) == 0 && scalar::toPcm(-inf) == 0, "invalid samples become silence");
    check(scalar::toPcm(2.f) == 32767 && scalar::toPcm(-2.f) == -32767, "PCM saturates safely");
    check(FilterModelDetail::clampAudio(nan) == 0 && FilterModelDetail::clampAudio(inf) == 0, "DSP finite recovery remains enabled");
    DualFilter filter; filter.prepare(48000); filter.setRouting(static_cast<FilterRouting>(999));
    const auto bypass = filter.processSample(.2f, -.3f, 0, 0);
    check(close(bypass.first, .2f) && close(bypass.second, -.3f), "unknown routing falls back to initialized bypass output");

    GranularOsc a, b;
    a.prepare(48000); b.prepare(48000);
    GranularOsc::Params p; p.bodyType = GranularOsc::BodyType::Glass; p.bodyMix = 1;
    p.density = 1200; p.jitter = 0;
    a.setParams(p); b.setParams(p); a.setFrequency(220); b.setFrequency(220);
    a.noteOn(57, 1); b.noteOn(57, 1);
    for (int i = 0; i < 1024; ++i) { a.processSample(); b.processSample(); }
    a.setFrequency(440); b.setFrequency(440);
    auto changed = p; changed.bodyType = GranularOsc::BodyType::Metal;
    b.setParams(changed); b.setParams(p); // explicit rebuild reference
    for (int i = 0; i < 1024; ++i)
    {
        auto x = a.processSample(), y = b.processSample();
        check(close(x.first, y.first) && close(x.second, y.second), "continuous pitch invalidates granular body coefficients");
    }
}
}

int main()
{
    transportTests(); routerTests(); envelopeTests(); voiceTests(); macroAndInputTests(); memoryAndSafetyTests();
    std::puts("control concurrency, MIDI/CV routing, note/pitch/engine transitions, envelope timing, macros, memory limits and PCM safety passed");
}
