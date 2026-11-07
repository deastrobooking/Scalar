#pragma once
// param_bridge.h — lock-free control→audio parameter handoff.
//
// The control task (core 0) edits a staging VoiceParams and publishes it;
// the audio task (core 1) picks up the latest snapshot at block boundaries.
// Double buffer + atomic sequence; the audio task never blocks.

#include "scalar/ScalarVoice.h"

#include <atomic>
#include <cstdint>

namespace scalar
{

class ParamBridge
{
public:
    // Control side: fill in a snapshot, then publish.
    VoiceParams& staging() noexcept { return buffers[writeIndex]; }

    void publish() noexcept
    {
        published.store(writeIndex, std::memory_order_release);
        writeIndex ^= 1;
        buffers[writeIndex] = buffers[writeIndex ^ 1];
    }

    // Audio side: returns true when a new snapshot is available.
    bool consume(VoiceParams& out) noexcept
    {
        const int idx = published.load(std::memory_order_acquire);
        if (idx == lastConsumed)
            return false;
        lastConsumed = idx;
        out = buffers[idx];
        return true;
    }

    // Continuous per-block signals (audio task reads every block).
    std::atomic<float> pitchHz { 220.f };
    std::atomic<bool>  gate { false };
    std::atomic<float> velocity { 1.f };

private:
    VoiceParams buffers[2] {};
    int writeIndex = 0;
    int lastConsumed = -1;
    std::atomic<int> published { -1 };
};

ParamBridge& paramBridge() noexcept;

} // namespace scalar
