#include "audio_task.h"
#include "board.h"
#include "param_bridge.h"
#include "scalar/AudioUtils.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <atomic>

namespace
{
constexpr const char* TAG = "audio";
constexpr uint32_t kBlockUs = board::kBlockFrames * 1000000 / board::kSampleRate;
i2s_chan_handle_t txChan = nullptr;
scalar::ScalarVoice voice;
std::atomic<uint32_t> maxRenderUs {0}, deadlineMisses {0}, writeFailures {0}, dmaQueueOverflows {0};
constexpr int16_t silence[board::kBlockFrames * 2 * 4] {};

bool dmaQueueOverflow(i2s_chan_handle_t, i2s_event_data_t*, void*)
{
    dmaQueueOverflows.fetch_add(1, std::memory_order_relaxed);
    return false;
}

esp_err_t preloadSilence()
{
    size_t loaded = 0;
    const auto result = i2s_channel_preload_data(txChan, silence, sizeof(silence), &loaded);
    return result == ESP_OK && loaded != sizeof(silence) ? ESP_FAIL : result;
}

esp_err_t initI2s()
{
    i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chanCfg.dma_desc_num = 4;
    chanCfg.dma_frame_num = board::kBlockFrames;
    chanCfg.auto_clear = true; // underruns output silence rather than replaying stale samples
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chanCfg, &txChan, nullptr), TAG, "channel allocation");
    i2s_std_config_t config = {
        .clk_cfg = {
            .sample_rate_hz = board::kSampleRate,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .ext_clk_freq_hz = 0,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
        },
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = board::kI2sBclk,
            .ws = board::kI2sLrclk,
            .dout = board::kI2sDout,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(txChan, &config), TAG, "standard mode");
    i2s_event_callbacks_t callbacks {};
    callbacks.on_send_q_ovf = dmaQueueOverflow;
    ESP_RETURN_ON_ERROR(i2s_channel_register_event_callback(txChan, &callbacks, nullptr), TAG, "DMA callbacks");
    return preloadSilence();
}

void audioLoop(void*)
{
    auto& bridge = scalar::paramBridge();
    scalar::VoiceParams params;
    int16_t block[board::kBlockFrames * 2];
    bool running = false;
    uint64_t renderedFrames = 0;
    ESP_LOGI(TAG, "audio task running on core %d", xPortGetCoreID());
    for (;;)
    {
        if (!running)
        {
            if (i2s_channel_enable(txChan) != ESP_OK)
            {
                writeFailures.fetch_add(1, std::memory_order_relaxed);
                vTaskDelay(pdMS_TO_TICKS(10));
                continue;
            }
            running = true;
        }
        const int64_t started = esp_timer_get_time();
        if (bridge.silenceAndFlushRequested()) voice.reset();
        if (bridge.consume(params)) voice.setParams(params);
        for (int i = 0; i < board::kBlockFrames; ++i)
        {
            // Consume one event per sample: even queued on/off/on events get
            // distinct render steps instead of collapsing into a gate level.
            scalar::VoiceEvent event;
            if (bridge.receiveAt(event, renderedFrames, board::kSampleRate))
            {
                switch (event.type)
                {
                    case scalar::VoiceEventType::Pitch: voice.setFrequency(event.frequency); break;
                    case scalar::VoiceEventType::NoteOn: voice.noteOn(event.note, event.velocity, event.frequency); break;
                    case scalar::VoiceEventType::NoteOff: voice.noteOff(); break;
                }
            }
            auto [l, r] = voice.processSample();
            block[2 * i] = scalar::toPcm(l);
            block[2 * i + 1] = scalar::toPcm(r);
            ++renderedFrames;
            if (!voice.isActive() && bridge.eventsEmpty()) bridge.resetTimeline();
        }
        const uint32_t elapsed = uint32_t(esp_timer_get_time() - started);
        if (elapsed > maxRenderUs.load(std::memory_order_relaxed)) maxRenderUs.store(elapsed, std::memory_order_relaxed);
        if (elapsed > kBlockUs) deadlineMisses.fetch_add(1, std::memory_order_relaxed);
        size_t written = 0;
        const auto result = i2s_channel_write(txChan, block, sizeof(block), &written, 5);
        if (result != ESP_OK || written != sizeof(block))
        {
            writeFailures.fetch_add(1, std::memory_order_relaxed);
            // Preserve musical state while restarting the transport with silence.
            // Counters are read/logged by control; never print from this path.
            const auto disabled = i2s_channel_disable(txChan);
            running = false;
            bridge.resetTimeline();
            if (disabled != ESP_OK && disabled != ESP_ERR_INVALID_STATE)
                vTaskDelay(pdMS_TO_TICKS(10));
            if (preloadSilence() != ESP_OK) vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}
}

bool audio_task_start()
{
    // All allocation/FFT/source generation precedes DMA playback and input tasks.
    voice.prepare(board::kSampleRate, [] { vTaskDelay(1); });
    if (initI2s() != ESP_OK)
    {
        if (txChan) i2s_del_channel(txChan);
        txChan = nullptr;
        ESP_LOGE(TAG, "I2S initialization failed");
        return false;
    }
#if CONFIG_FREERTOS_UNICORE
    constexpr BaseType_t core = 0;
#else
    constexpr BaseType_t core = 1;
#endif
    if (xTaskCreatePinnedToCore(audioLoop, "audio", 16384, nullptr, configMAX_PRIORITIES - 2, nullptr, core) == pdPASS)
        return true;
    i2s_del_channel(txChan);
    txChan = nullptr;
    ESP_LOGE(TAG, "audio task allocation failed");
    return false;
}

AudioTaskStats audio_task_stats()
{
    return {maxRenderUs.load(std::memory_order_relaxed), deadlineMisses.load(std::memory_order_relaxed),
        writeFailures.load(std::memory_order_relaxed), dmaQueueOverflows.load(std::memory_order_relaxed)};
}
