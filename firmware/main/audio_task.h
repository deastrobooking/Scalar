#pragma once
#include <cstdint>

struct AudioTaskStats
{
    uint32_t maxRenderUs = 0, deadlineMisses = 0, writeFailures = 0, dmaQueueOverflows = 0;
};
bool audio_task_start();
AudioTaskStats audio_task_stats();
