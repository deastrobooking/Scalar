// Scalar — stripped-down GLIESE 710 voice as a Eurorack module.
// ESP32-S3 entry point: control on core 0, audio on core 1.

#include "audio_task.h"
#include "control_task.h"
#include "usb_midi.h"

#include "esp_log.h"
#include <cstdlib>

extern "C" void app_main()
{
    ESP_LOGI("scalar", "Scalar starting");
    if (!audio_task_start() || !control_task_start())
    {
        ESP_LOGE("scalar", "required audio/control initialization failed");
        std::abort();
    }
    usb_midi_start();
}
