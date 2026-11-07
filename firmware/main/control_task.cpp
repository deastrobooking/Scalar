#include "control_task.h"
#include "audio_task.h"
#include "board.h"
#include "param_bridge.h"
#include "scalar/ControlRouter.h"
#include "scalar/ControlUtils.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include <cmath>
#include <cstring>

namespace
{
constexpr const char* TAG = "control";
constexpr int kInputCapacity = 128;
enum class InputType { Gate, Midi, Disconnect, PitchVolts, InternalPitch, Calibration };
struct Input
{
    InputType type {};
    uint8_t packet[4] {};
    bool gate = false;
    float value = 0.f, scale = 0.f;
    uint64_t timeUs = 0;
};
QueueHandle_t inputs = nullptr;
std::atomic<bool> inputOverflow {false};
adc_oneshot_unit_handle_t adc1 = nullptr;
adc_cali_handle_t adcCalibration = nullptr;
scalar::ParamBridge bridgeInstance;
uint32_t adcErrors = 0;

bool post(const Input& input)
{
    if (!inputs) return false;
    Input captured = input;
    captured.timeUs = esp_timer_get_time();
    if (xQueueSend(inputs, &captured, 0) == pdTRUE) return true;
    inputOverflow.store(true, std::memory_order_release);
    return false;
}

void gateInterrupt(void*)
{
    Input input;
    input.type = InputType::Gate;
    input.gate = gpio_get_level(board::kGateIn) != 0;
    input.timeUs = esp_timer_get_time();
    BaseType_t wake = pdFALSE;
    if (xQueueSendFromISR(inputs, &input, &wake) != pdTRUE)
        inputOverflow.store(true, std::memory_order_release);
    if (wake) portYIELD_FROM_ISR();
}

struct Smoothed
{
    float value = 0.f;
    bool initialized = false;
    float update(float x, float alpha = 0.35f)
    {
        if (!initialized) { value = x; initialized = true; }
        else value += alpha * (x - value);
        return value;
    }
};

bool readAdc(int channel, int& raw)
{
    if (adc_oneshot_read(adc1, static_cast<adc_channel_t>(channel), &raw) == ESP_OK) return true;
    ++adcErrors;
    return false;
}

void initInputs()
{
    adc_oneshot_unit_init_cfg_t unitCfg {};
    unitCfg.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&unitCfg, &adc1));
    adc_oneshot_chan_cfg_t chanCfg = { .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_12 };
    for (int ch : {board::kAdcVOct, board::kAdcCutoff, board::kAdcRes, board::kAdcTimbre, board::kAdcMorph})
        ESP_ERROR_CHECK(adc_oneshot_config_channel(adc1, static_cast<adc_channel_t>(ch), &chanCfg));
    adc_cali_curve_fitting_config_t calibrationCfg = {
        .unit_id = ADC_UNIT_1,
        .chan = static_cast<adc_channel_t>(board::kAdcVOct),
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    const esp_err_t calibrationResult = adc_cali_create_scheme_curve_fitting(&calibrationCfg, &adcCalibration);
    if (calibrationResult != ESP_OK)
        ESP_LOGW(TAG, "ADC calibration unavailable (%s); internal pitch holds its last value", esp_err_to_name(calibrationResult));

    gpio_config_t gateCfg = {
        .pin_bit_mask = 1ULL << board::kGateIn,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&gateCfg));
    gpio_config_t buttonCfg = {
        .pin_bit_mask = 1ULL << board::kEngineButton,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&buttonCfg));
}

void controlLoop(void*)
{
    initInputs();
    scalar::ControlRouter router(bridgeInstance, board::kMidiChannel);
    scalar::PitchCalibration pitchCalibration {board::kPitchZeroMv, board::kPitchMvPerVolt};
    scalar::DebouncedButton button;
    Smoothed pitch, cutoff, resonance, timbre, morph;
    bool externalPitch = false;
    int engineIndex = 0;
    uint32_t tick = 0;
    TickType_t wake = xTaskGetTickCount();
    router.setInputTime(esp_timer_get_time());
    router.setCvGate(gpio_get_level(board::kGateIn) != 0);
    ESP_ERROR_CHECK(gpio_install_isr_service(0));
    ESP_ERROR_CHECK(gpio_isr_handler_add(board::kGateIn, gateInterrupt, nullptr));
    ESP_ERROR_CHECK(gpio_set_intr_type(board::kGateIn, GPIO_INTR_ANYEDGE));
    // Reconcile a level change between the initial read and interrupt enable.
    router.setCvGate(gpio_get_level(board::kGateIn) != 0);
    ESP_LOGI(TAG, "control task running on core %d", xPortGetCoreID());

    for (;;)
    {
        router.setInputTime(esp_timer_get_time());
        if (bridgeInstance.recoveryReady())
        {
            router.reset();
            bridgeInstance.acknowledgeRecovery();
            xQueueReset(inputs);
            router.setCvGate(gpio_get_level(board::kGateIn) != 0);
        }
        if (inputOverflow.exchange(false, std::memory_order_acq_rel))
        {
            xQueueReset(inputs);
            bridgeInstance.send({scalar::VoiceEventType::NoteOff});
            router.reset();
            router.setCvGate(gpio_get_level(board::kGateIn) != 0);
            ESP_LOGW(TAG, "input queue overflow: cleared held MIDI notes and resumed CV");
        }

        int raw = 0, millivolts = 0;
        float volts = 0.f;
        if (!externalPitch && adcCalibration && readAdc(board::kAdcVOct, raw))
        {
            if (adc_cali_raw_to_voltage(adcCalibration, raw, &millivolts) == ESP_OK
                && pitchCalibration.convert(float(millivolts), volts))
                router.setCvPitch(130.81278f * std::exp2(pitch.update(volts, 0.5f)));
            else ++adcErrors;
        }

        Input input;
        // Bounded drain: sustained incoming traffic cannot starve the next tick.
        for (int i = 0; i < kInputCapacity && xQueueReceive(inputs, &input, 0) == pdTRUE; ++i)
        {
            router.setInputTime(input.timeUs);
            switch (input.type)
            {
                case InputType::Gate: router.setCvGate(input.gate); break;
                case InputType::Midi: router.midi(input.packet); break;
                case InputType::Disconnect: router.disconnect(); break;
                case InputType::PitchVolts:
                    externalPitch = true;
                    router.setCvPitch(130.81278f * std::exp2(input.value));
                    break;
                case InputType::InternalPitch: externalPitch = false; pitch.initialized = false; break;
                case InputType::Calibration:
                    pitchCalibration = {input.value, input.scale};
                    pitch.initialized = false;
                    break;
            }
        }
        const uint32_t nowMs = uint32_t(esp_timer_get_time() / 1000);
        if (button.update(gpio_get_level(board::kEngineButton) != 0, nowMs))
            engineIndex = (engineIndex + 1) % int(scalar::Engine::NumEngines);

        if (tick++ % 10 == 0)
        {
            auto& p = bridgeInstance.staging();
            p.engine = static_cast<scalar::Engine>(engineIndex);
            p.filterA.mode = FilterMode::LP24;
            if (readAdc(board::kAdcCutoff, raw)) p.filterA.cutoff = 20.f * std::exp2(cutoff.update(raw / 4095.f) * 10.f);
            if (readAdc(board::kAdcRes, raw)) p.filterA.resonance = resonance.update(raw / 4095.f);
            if (readAdc(board::kAdcTimbre, raw)) timbre.update(raw / 4095.f);
            if (readAdc(board::kAdcMorph, raw)) morph.update(raw / 4095.f);
            scalar::applyMacros(p, timbre.value, morph.value);
            bridgeInstance.publish();
        }
        if (tick % 1000 == 0)
        {
            const auto stats = audio_task_stats();
            ESP_LOGD(TAG, "render max=%lu us late=%lu writes=%lu DMA queue=%lu events=%lu ADC=%lu",
                (unsigned long)stats.maxRenderUs, (unsigned long)stats.deadlineMisses,
                (unsigned long)stats.writeFailures, (unsigned long)stats.dmaQueueOverflows,
                (unsigned long)bridgeInstance.overflowCount(), (unsigned long)adcErrors);
        }
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(1));
    }
}
}

namespace scalar { ParamBridge& paramBridge() noexcept { return bridgeInstance; } }

bool control_task_start()
{
    inputs = xQueueCreate(kInputCapacity, sizeof(Input));
    if (!inputs) { ESP_LOGE(TAG, "input queue allocation failed"); return false; }
    if (xTaskCreatePinnedToCore(controlLoop, "control", 8192, nullptr, 5, nullptr, 0) == pdPASS) return true;
    vQueueDelete(inputs);
    inputs = nullptr;
    ESP_LOGE(TAG, "control task allocation failed");
    return false;
}

bool control_post_midi(const uint8_t packet[4])
{
    Input input; input.type = InputType::Midi;
    std::memcpy(input.packet, packet, 4);
    return post(input);
}
void control_midi_disconnect() { Input input; input.type = InputType::Disconnect; post(input); }
namespace cv
{
bool setPitchVolts(float volts)
{
    if (!std::isfinite(volts) || volts < -5.f || volts > 5.f) return false;
    Input input; input.type = InputType::PitchVolts; input.value = volts;
    return post(input);
}
bool useInternalPitch() { Input input; input.type = InputType::InternalPitch; return post(input); }
bool setPitchCalibration(float zeroMv, float millivoltsPerVolt)
{
    if (!std::isfinite(zeroMv) || !std::isfinite(millivoltsPerVolt) || millivoltsPerVolt <= 0.f) return false;
    Input input; input.type = InputType::Calibration; input.value = zeroMv; input.scale = millivoltsPerVolt;
    return post(input);
}
}
