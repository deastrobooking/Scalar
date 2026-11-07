// usb_midi.cpp — class-compliant USB MIDI device via esp_tinyusb.
//
// Note on/off and pitch bend drive the voice through the ParamBridge; MIDI
// takes priority over the CV/gate inputs while notes are held.

#include "usb_midi.h"
#include "param_bridge.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "tinyusb.h"
#include "tusb.h"

#include <cmath>

static const char* TAG = "usb_midi";

namespace
{
int activeNote = -1;
float bendSemis = 0.f;

void applyPitch()
{
    if (activeNote < 0)
        return;
    const float hz = 440.f * std::exp2((static_cast<float>(activeNote) - 69.f + bendSemis) / 12.f);
    scalar::paramBridge().pitchHz.store(hz, std::memory_order_relaxed);
}

void handlePacket(const uint8_t packet[4])
{
    const uint8_t status = packet[1] & 0xF0;
    auto& bridge = scalar::paramBridge();

    switch (status)
    {
        case 0x90: // note on
            if (packet[3] > 0)
            {
                activeNote = packet[2];
                bridge.velocity.store(static_cast<float>(packet[3]) / 127.f,
                                      std::memory_order_relaxed);
                applyPitch();
                bridge.gate.store(true, std::memory_order_relaxed);
                break;
            }
            [[fallthrough]]; // velocity 0 == note off
        case 0x80: // note off
            if (packet[2] == activeNote)
            {
                activeNote = -1;
                bridge.gate.store(false, std::memory_order_relaxed);
            }
            break;
        case 0xE0: // pitch bend, ±2 semitones
        {
            const int value = packet[2] | (packet[3] << 7);
            bendSemis = (static_cast<float>(value) - 8192.f) / 8192.f * 2.f;
            applyPitch();
            break;
        }
        default:
            break;
    }
}

void midiLoop(void*)
{
    uint8_t packet[4];
    for (;;)
    {
        while (tud_midi_available())
            if (tud_midi_packet_read(packet))
                handlePacket(packet);
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
} // namespace

void usb_midi_start()
{
    const tinyusb_config_t tusbCfg = {}; // default descriptors expose MIDI
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusbCfg));
    xTaskCreatePinnedToCore(midiLoop, "usb_midi", 4096, nullptr, 5, nullptr, 0);
    ESP_LOGI(TAG, "USB MIDI ready");
}
