idf_component_register(
    SRCS
        main.cpp
        audio_task.cpp
        control_task.cpp
        usb_midi.cpp
    INCLUDE_DIRS .
    REQUIRES scalar_dsp driver esp_adc
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
