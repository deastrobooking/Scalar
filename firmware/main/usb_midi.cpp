#include "usb_midi.h"
#include "control_task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "tinyusb.h"
#include "tusb.h"
#include <cstdio>

namespace
{
constexpr const char* TAG = "usb_midi";
static_assert(CFG_TUD_MIDI == 1, "Scalar descriptors define exactly one MIDI interface");
constexpr uint8_t configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_MIDI_DESC_LEN, 0, 100),
    TUD_MIDI_DESCRIPTOR(0, 4, 0x01, 0x81, 64),
};
const char language[] = {0x09, 0x04};
char serial[13] {};
const char* strings[] = {language, "Scalar", "Scalar USB MIDI", serial, "Scalar MIDI"};

}

// Runs in TinyUSB's task, immediately after packet receipt. Avoid an extra
// polling tick that would collapse arrival times of short MIDI notes.
extern "C" void tud_midi_rx_cb(uint8_t)
{
    uint8_t packet[4];
    // RX FIFO is finite and filled by this same USB task; drain every packet.
    while (tud_midi_packet_read(packet)) control_post_midi(packet);
}
extern "C" void tud_umount_cb() { control_midi_disconnect(); }
extern "C" void tud_suspend_cb(bool) { control_midi_disconnect(); }

bool usb_midi_start()
{
    uint8_t mac[6];
    if (esp_read_mac(mac, ESP_MAC_BASE) != ESP_OK) return false;
    std::snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    tinyusb_config_t config {};
    config.string_descriptor = strings;
    config.string_descriptor_count = sizeof(strings) / sizeof(strings[0]);
    config.configuration_descriptor = configuration;
    const auto result = tinyusb_driver_install(&config);
    if (result != ESP_OK)
    {
        ESP_LOGW(TAG, "USB unavailable (%s); CV/audio remain active", esp_err_to_name(result));
        return false;
    }
    ESP_LOGI(TAG, "USB MIDI ready (channel 1)");
    return true;
}
