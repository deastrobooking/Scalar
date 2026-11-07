#pragma once
// board.h — Scalar module pin map (ESP32-S3-WROOM-1 devkit baseline).
// Adjust to the final panel PCB.

#include "driver/gpio.h"
#include "sdkconfig.h"

#if !CONFIG_IDF_TARGET_ESP32S3
#error "This board profile requires ESP32-S3; add and validate a board profile for other targets."
#endif
static_assert(CONFIG_FREERTOS_HZ >= 1000, "Scalar controls require a 1 kHz RTOS tick");

namespace board
{
// ─── I2S audio out → PCM5102A ────────────────────────────────────────────────
constexpr gpio_num_t kI2sBclk  = GPIO_NUM_15;
constexpr gpio_num_t kI2sLrclk = GPIO_NUM_16;
constexpr gpio_num_t kI2sDout  = GPIO_NUM_17;

// ─── Gate input (Eurorack gate, conditioned to 3.3 V logic) ──────────────────
constexpr gpio_num_t kGateIn = GPIO_NUM_18;

// ─── CV / pot inputs (ADC1 channels) ─────────────────────────────────────────
// NOTE: the internal 12-bit ADC is fine for panel pots and mod CVs but is
// marginal for accurate V/Oct tracking. For production, route V/Oct through
// an external ADC and feed cv::setPitchVolts() in control_task.h.
constexpr int kAdcVOct    = 0; // ADC1_CH0 — GPIO1 — V/Oct (±5 V scaled to 0-3.0 V)
constexpr float kPitchZeroMv = 1500.f;  // measure at 0 V input
constexpr float kPitchMvPerVolt = 300.f; // measured (+1 V reading - 0 V reading)
constexpr int kAdcCutoff  = 3; // ADC1_CH3 — GPIO4 — filter cutoff pot
constexpr int kAdcRes     = 4; // ADC1_CH4 — GPIO5 — resonance pot
constexpr int kAdcTimbre  = 5; // ADC1_CH5 — GPIO6 — engine timbre pot
constexpr int kAdcMorph   = 6; // ADC1_CH6 — GPIO7 — engine morph pot

// ─── Engine select encoder/button ────────────────────────────────────────────
constexpr gpio_num_t kEngineButton = GPIO_NUM_0; // boot button on devkits
// GPIO0 is a boot strap: use a non-strapping GPIO on the production PCB.
constexpr uint8_t kMidiChannel = 0; // MIDI channel 1

// ─── Audio format ────────────────────────────────────────────────────────────
constexpr int kSampleRate  = 48000;
constexpr int kBlockFrames = 64;   // 1.33 ms per block
} // namespace board
