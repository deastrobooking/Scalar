// audio_task.cpp — hard real-time render loop, pinned to core 1.
//
// Pulls the latest VoiceParams snapshot at block boundaries, renders
// board::kBlockFrames stereo samples through ScalarVoice, converts to int16
// and pushes them to the I2S DMA (PCM5102A, Philips format).

#include "audio_task.h"
#include "board.h"
#include "param_bridge.h"

#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* TAG = "audio";

namespace
{
i2s_chan_handle_t txChan = nullptr;
scalar::ScalarVoice voice;

void initI2s()
{
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 4;
    chanCfg.dma_frame_num = board::kBlockFrames;
    ESP_ERROR_CHECK(i2s_new_channel(&chanCfg, &txChan, nullptr));

    i2s_std_config_t stdCfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(board::kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                        I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, // PCM5102A generates its own from BCK
            .bclk = board::kI2sBclk,
            .ws   = board::kI2sLrclk,
            .dout = board::kI2sDout,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(txChan, &stdCfg));
    ESP_ERROR_CHECK(i2s_channel_enable(txChan));
}

inline int16_t toPcm(float s) noexcept
{
    s = s < -1.f ? -1.f : (s > 1.f ? 1.f : s);
    return static_cast<int16_t>(s * 32767.f);
}

void audioLoop(void*)
{
    initI2s();

    voice.prepare(board::kSampleRate);

    auto& bridge = scalar::paramBridge();
    scalar::VoiceParams params;
    bool lastGate = false;
    float lastPitch = -1.f;

    int16_t block[board::kBlockFrames * 2];

    ESP_LOGI(TAG, "audio task running on core %d", xPortGetCoreID());

    for (;;)
    {
        // Block-rate control pickup — no locks, no allocation.
        if (bridge.consume(params))
            voice.setParams(params);

        const float pitch = bridge.pitchHz.load(std::memory_order_relaxed);
        if (pitch != lastPitch)
        {
            voice.setFrequency(pitch);
            lastPitch = pitch;
        }

        const bool gate = bridge.gate.load(std::memory_order_relaxed);
        if (gate && !lastGate)
        {
            const float vel = bridge.velocity.load(std::memory_order_relaxed);
            // Derive a MIDI-ish note number from pitch for engines that want it.
            const int note = static_cast<int>(69.f + 12.f * std::log2(pitch / 440.f) + 0.5f);
            voice.noteOn(note, vel);
        }
        else if (!gate && lastGate)
        {
            voice.noteOff();
        }
        lastGate = gate;

        for (int i = 0; i < board::kBlockFrames; ++i)
        {
            auto [l, r] = voice.processSample();
            block[i * 2]     = toPcm(l);
            block[i * 2 + 1] = toPcm(r);
        }

        size_t written = 0;
        i2s_channel_write(txChan, block, sizeof(block), &written, portMAX_DELAY);
    }
}
} // namespace

void audio_task_start()
{
    // Core 1, highest practical priority: the render loop paces itself on the
    // I2S DMA, so it naturally yields while DMA drains.
    xTaskCreatePinnedToCore(audioLoop, "audio", 16384, nullptr,
                            configMAX_PRIORITIES - 2, nullptr, 1);
}
