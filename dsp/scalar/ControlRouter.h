#pragma once
#include "ControlTransport.h"
#include <algorithm>
#include <cmath>

namespace scalar
{
// Control-task-owned last-note-priority mono router. Every note-on retriggers.
// CV state keeps tracking while MIDI owns the voice, then resumes at velocity 1.
class ControlRouter
{
public:
    explicit ControlRouter(ControlTransport& output, uint8_t midiChannel = 0)
        : bridge(output), channel(midiChannel) {}
    void setInputTime(uint64_t timeUs) noexcept { inputTimeUs = timeUs; }
    void setCvPitch(float hz)
    {
        if (!std::isfinite(hz) || hz <= 0.f || hz == cvHz) return;
        cvHz = hz;
        if (heldCount == 0) emit(VoiceEventType::Pitch, cvHz, 1.f, cvNote());
    }
    void setCvGate(bool high)
    {
        if (high == cvGate) return;
        cvGate = high;
        if (heldCount == 0)
            emit(high ? VoiceEventType::NoteOn : VoiceEventType::NoteOff, cvHz, 1.f, cvNote());
    }
    void midi(const uint8_t packet[4])
    {
        if ((packet[0] >> 4) != 0 || (packet[1] & 0x0f) != channel) return;
        const uint8_t status = packet[1] & 0xf0;
        const uint8_t cin = packet[0] & 0x0f;
        if (cin != (status >> 4) || packet[2] > 127 || packet[3] > 127) return;
        const uint8_t note = packet[2];
        if (status == 0x90 && packet[3] != 0)
        {
            remove(note);
            held[heldCount++] = note;
            velocities[note] = packet[3] / 127.f;
            playMidi();
        }
        else if (status == 0x80 || status == 0x90)
        {
            const bool active = heldCount != 0 && held[heldCount - 1] == note;
            remove(note);
            if (active) { if (heldCount) playMidi(); else resumeCv(); }
        }
        else if (status == 0xe0)
        {
            bend = ((packet[2] | (packet[3] << 7)) - 8192) * (2.f / 8192.f);
            if (heldCount) emit(VoiceEventType::Pitch, midiHz(), velocities[held[heldCount - 1]], held[heldCount - 1]);
        }
        else if (status == 0xb0 && (packet[2] == 120 || packet[2] == 123)) disconnect();
        else if (status == 0xb0 && packet[2] == 121)
        {
            bend = 0.f;
            if (heldCount) emit(VoiceEventType::Pitch, midiHz(), velocities[held[heldCount - 1]], held[heldCount - 1]);
        }
    }
    void disconnect()
    {
        const bool wasMidi = heldCount != 0;
        heldCount = 0;
        bend = 0.f;
        if (wasMidi) resumeCv();
    }
    // Call after the audio-side overflow acknowledgement (audio is silent).
    void reset() noexcept { heldCount = 0; bend = 0.f; cvGate = false; }
private:
    ControlTransport& bridge;
    uint8_t channel;
    std::array<uint8_t, 128> held {};
    std::array<float, 128> velocities {};
    size_t heldCount = 0;
    float cvHz = 130.81278f, bend = 0.f;
    bool cvGate = false;
    uint64_t inputTimeUs = 0;
    int cvNote() const { return std::clamp(int(std::lround(69.f + 12.f * std::log2(cvHz / 440.f))), 0, 127); }
    float midiHz() const { return 440.f * std::exp2((held[heldCount - 1] - 69.f + bend) / 12.f); }
    void emit(VoiceEventType type, float hz, float velocity, int note) { bridge.send({type, hz, velocity, note, inputTimeUs}); }
    void remove(uint8_t note)
    {
        for (size_t i = 0; i < heldCount; ++i)
            if (held[i] == note)
            {
                for (size_t j = i + 1; j < heldCount; ++j) held[j - 1] = held[j];
                --heldCount;
                break;
            }
    }
    void playMidi() { emit(VoiceEventType::NoteOn, midiHz(), velocities[held[heldCount - 1]], held[heldCount - 1]); }
    void resumeCv()
    {
        emit(VoiceEventType::NoteOff, cvHz, 1.f, cvNote());
        emit(cvGate ? VoiceEventType::NoteOn : VoiceEventType::Pitch, cvHz, 1.f, cvNote());
    }
};
}
