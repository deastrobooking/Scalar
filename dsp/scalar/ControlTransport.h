#pragma once
#include "ScalarVoice.h"
#include <atomic>
#include <array>
#include <cstdint>

namespace scalar
{
// Each slot belongs to either producer, consumer, or the atomic middle index.
// Neither side touches the other's slot, including while a copy is in flight.
template<class T>
class LatestSnapshot
{
public:
    T& staging() noexcept { return staged; }
    void publish() noexcept
    {
        slots[writer] = staged;
        writer = middle.exchange(writer | kDirty, std::memory_order_acq_rel) & kIndex;
    }
    bool consume(T& out) noexcept
    {
        if (!(middle.load(std::memory_order_acquire) & kDirty)) return false;
        reader = middle.exchange(reader, std::memory_order_acq_rel) & kIndex;
        out = slots[reader];
        return true;
    }
private:
    static constexpr uint32_t kDirty = 4, kIndex = 3;
    std::array<T, 3> slots {};
    T staged {};
    uint32_t writer = 0, reader = 1;
    std::atomic<uint32_t> middle { 2 };
};

enum class VoiceEventType { Pitch, NoteOn, NoteOff };
struct VoiceEvent
{
    VoiceEventType type = VoiceEventType::NoteOff;
    float frequency = 130.81278f;
    float velocity = 1.f;
    int note = 48;
    uint64_t timeUs = 0; // captured input time; zero means deliver immediately
};

template<class T, size_t Capacity>
class SpscQueue
{
public:
    bool push(const T& value) noexcept
    {
        const uint32_t head = write.load(std::memory_order_relaxed);
        const uint32_t next = (head + 1) % (Capacity + 1);
        if (next == read.load(std::memory_order_acquire)) return false;
        data[head] = value;
        write.store(next, std::memory_order_release);
        return true;
    }
    bool pop(T& value) noexcept
    {
        const uint32_t tail = read.load(std::memory_order_relaxed);
        if (tail == write.load(std::memory_order_acquire)) return false;
        value = data[tail];
        read.store((tail + 1) % (Capacity + 1), std::memory_order_release);
        return true;
    }
    bool peek(T& value) const noexcept
    {
        const uint32_t tail = read.load(std::memory_order_relaxed);
        if (tail == write.load(std::memory_order_acquire)) return false;
        value = data[tail];
        return true;
    }
private:
    std::array<T, Capacity + 1> data {};
    std::atomic<uint32_t> read { 0 }, write { 0 };
};

class ControlTransport : public LatestSnapshot<VoiceParams>
{
public:
    static constexpr size_t kEventCapacity = 128;
    // Producer stops on overflow until audio has flushed the queue and muted.
    bool send(const VoiceEvent& event) noexcept
    {
        if (fault.load(std::memory_order_acquire) != Healthy) return false;
        if (events.push(event)) return true;
        overflows.fetch_add(1, std::memory_order_relaxed);
        fault.store(Requested, std::memory_order_release);
        return false;
    }
    bool receive(VoiceEvent& event) noexcept { return events.pop(event); }
    // Consumer's sample clock preserves captured note durations even when a
    // control tick delivered several edges together. Pitch remains continuous.
    bool receiveAt(VoiceEvent& event, uint64_t frame, uint32_t sampleRate) noexcept
    {
        if (!events.peek(event)) return false;
        if (event.type != VoiceEventType::Pitch && event.timeUs != 0)
        {
            if (!timelineSet)
            {
                originUs = event.timeUs;
                originFrame = frame;
                timelineSet = true;
            }
            const uint64_t delta = event.timeUs > originUs ? event.timeUs - originUs : 0;
            const uint64_t due = originFrame + (delta / 1000000) * sampleRate
                + (delta % 1000000) * sampleRate / 1000000;
            if (frame < due) return false;
        }
        return events.pop(event);
    }
    bool eventsEmpty() const noexcept { VoiceEvent event; return !events.peek(event); }
    void resetTimeline() noexcept { timelineSet = false; }
    // Consumer only. Producer cannot reuse the queue until acknowledgeRecovery.
    bool silenceAndFlushRequested() noexcept
    {
        if (fault.load(std::memory_order_acquire) != Requested) return false;
        VoiceEvent ignored;
        while (events.pop(ignored)) {}
        resetTimeline();
        fault.store(Acknowledged, std::memory_order_release);
        return true;
    }
    // Producer only. Reset its router before admitting further input events.
    bool recoveryReady() const noexcept
    {
        return fault.load(std::memory_order_acquire) == Acknowledged;
    }
    void acknowledgeRecovery() noexcept { fault.store(Healthy, std::memory_order_release); }
    uint32_t overflowCount() const noexcept { return overflows.load(std::memory_order_relaxed); }
private:
    enum : uint32_t { Healthy, Requested, Acknowledged };
    SpscQueue<VoiceEvent, kEventCapacity> events;
    std::atomic<uint32_t> fault { Healthy }, overflows { 0 };
    bool timelineSet = false;
    uint64_t originUs = 0, originFrame = 0;
};
static_assert(std::atomic<uint32_t>::is_always_lock_free);
}
