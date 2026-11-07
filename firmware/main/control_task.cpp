// control_task.cpp — control-rate loop on core 0 (~1 kHz).
//
// Reads gate GPIO, CV/pot ADC channels and the engine-select button, maps them
// onto VoiceParams, and publishes snapshots through the ParamBridge. USB MIDI
// events (usb_midi.cpp) also land here via the bridge's atomics.

#include "control_task.h"
#include "board.h"
#include "param_bridge.h"

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cmath>

static const char* TAG = "control";

namespace
{
adc_oneshot_unit_handle_t adc1;

scalar::ParamBridge bridgeInstance;

struct Smoothed
{
    float value = 0.f;
    float update(int raw) noexcept
    {
        // One-pole smoothing of 12-bit ADC reads (0..4095 → 0..1).
        const float x = static_cast<float>(raw) * (1.f / 4095.f);
        value += 0.15f * (x - value);
        return value;
    }
};

int readAdc(int channel)
{
    int raw = 0;
    adc_oneshot_read(adc1, static_cast<adc_channel_t>(channel), &raw);
    return raw;
}

void initInputs()
{
    adc_oneshot_unit_init_cfg_t unitCfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unitCfg, &adc1));

    adc_oneshot_chan_cfg_t chanCfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    for (int ch : { board::kAdcVOct, board::kAdcCutoff, board::kAdcRes,
                    board::kAdcTimbre, board::kAdcMorph })
        ESP_ERROR_CHECK(adc_oneshot_config_channel(
            adc1, static_cast<adc_channel_t>(ch), &chanCfg));

    gpio_config_t gateCfg = {
        .pin_bit_mask = 1ULL << board::kGateIn,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&gateCfg);

    gpio_config_t btnCfg = {
        .pin_bit_mask = 1ULL << board::kEngineButton,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&btnCfg);
}

// Map the shared timbre/morph macro pots onto whichever engine is active.
void applyMacros(scalar::VoiceParams& p, float timbre, float morph)
{
    switch (p.engine)
    {
        case scalar::Engine::VA:
            p.va.shapeMorph = timbre * 5.f;
            p.va.wavefold = morph;
            break;
        case scalar::Engine::Wavetable:
            p.wavetable.warpAmount = morph;
            // timbre drives table scan position via setTablePosition in future rev
            break;
        case scalar::Engine::FM:
            p.fmMorph = morph;
            p.fmAlgorithm = static_cast<int>(timbre * 39.f);
            break;
        case scalar::Engine::Osc3D:
            p.osc3d.morphT = morph;
            p.osc3d.fieldFreq = 0.25f + timbre * 7.75f;
            break;
        case scalar::Engine::Terrain:
            p.terrain.morph = morph;
            p.terrain.scanRate = 0.05f + timbre * 4.f;
            break;
        case scalar::Engine::Granular:
            p.granular.position = timbre;
            p.granular.sizeMs = 5.f + morph * 495.f;
            break;
        default:
            break;
    }
}

void controlLoop(void*)
{
    initInputs();

    Smoothed voct, cutoff, res, timbre, morph;
    int engineIndex = 0;
    bool lastButton = true; // pulled up
    uint32_t tick = 0;

    ESP_LOGI(TAG, "control task running on core %d", xPortGetCoreID());

    for (;;)
    {
        // Gate + pitch every tick (1 kHz)
        const bool gate = gpio_get_level(board::kGateIn) != 0;
        bridgeInstance.gate.store(gate, std::memory_order_relaxed);

        const float v = voct.update(readAdc(board::kAdcVOct));
        // Front-end scales ±5 V → 0..3.3 V; 0.5 ≙ 0 V ≙ C3 (130.81 Hz), 1 V/Oct.
        const float volts = (v - 0.5f) * 10.f;
        bridgeInstance.pitchHz.store(130.81f * std::exp2(volts),
                                     std::memory_order_relaxed);

        // Engine-select button (falling edge)
        const bool button = gpio_get_level(board::kEngineButton) != 0;
        if (lastButton && !button)
        {
            engineIndex = (engineIndex + 1) % static_cast<int>(scalar::Engine::NumEngines);
            ESP_LOGI(TAG, "engine -> %s",
                     scalar::engineName(static_cast<scalar::Engine>(engineIndex)));
        }
        lastButton = button;

        // Pots / mod CVs + publish at ~100 Hz
        if (++tick % 10 == 0)
        {
            auto& p = bridgeInstance.staging();
            p.engine = static_cast<scalar::Engine>(engineIndex);
            p.filterA.mode = FilterMode::LP24;
            p.filterA.cutoff = 20.f * std::exp2(cutoff.update(readAdc(board::kAdcCutoff)) * 10.f);
            p.filterA.resonance = res.update(readAdc(board::kAdcRes));
            applyMacros(p,
                        timbre.update(readAdc(board::kAdcTimbre)),
                        morph.update(readAdc(board::kAdcMorph)));
            bridgeInstance.publish();
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
} // namespace

namespace scalar
{
ParamBridge& paramBridge() noexcept { return bridgeInstance; }
}

void control_task_start()
{
    xTaskCreatePinnedToCore(controlLoop, "control", 8192, nullptr, 5, nullptr, 0);
}
