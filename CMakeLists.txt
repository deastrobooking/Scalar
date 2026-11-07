cmake_minimum_required(VERSION 3.22)
project(Scalar LANGUAGES CXX)

# Native host build of the Scalar DSP core: used for development, WAV
# rendering, and tests. The ESP32-S3 firmware build lives in firmware/
# (ESP-IDF) and consumes the same dsp/ sources as a component.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(scalar_dsp STATIC
    dsp/gliese/Oscillators/Osc3D.cpp
    dsp/gliese/Oscillators/TerrainOsc.cpp
    dsp/gliese/Oscillators/WavetableOsc.cpp
    dsp/gliese/Oscillators/FMOperator.cpp
    dsp/gliese/Oscillators/FMAlgorithm.cpp
    dsp/gliese/Oscillators/VAOscillator.cpp
    dsp/gliese/Oscillators/GranularOsc.cpp
    dsp/gliese/Envelopes/ADSREnvelope.cpp
    dsp/gliese/Filter/DualFilter.cpp
    dsp/scalar/ScalarVoice.cpp
)

target_include_directories(scalar_dsp PUBLIC dsp/shim dsp)
target_compile_options(scalar_dsp PRIVATE -Wall -Wextra)

add_executable(scalar_render tools/render/main.cpp)
target_link_libraries(scalar_render PRIVATE scalar_dsp)

enable_testing()
add_test(NAME render_smoke COMMAND scalar_render --smoke)
