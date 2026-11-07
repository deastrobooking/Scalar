# scalar_dsp — wraps the shared, platform-agnostic DSP core (../../dsp)
# as an ESP-IDF component.

set(SCALAR_DSP_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../dsp)

idf_component_register(
    SRCS
        ${SCALAR_DSP_ROOT}/gliese/Oscillators/Osc3D.cpp
        ${SCALAR_DSP_ROOT}/gliese/Oscillators/TerrainOsc.cpp
        ${SCALAR_DSP_ROOT}/gliese/Oscillators/WavetableOsc.cpp
        ${SCALAR_DSP_ROOT}/gliese/Oscillators/FMOperator.cpp
        ${SCALAR_DSP_ROOT}/gliese/Oscillators/FMAlgorithm.cpp
        ${SCALAR_DSP_ROOT}/gliese/Oscillators/VAOscillator.cpp
        ${SCALAR_DSP_ROOT}/gliese/Oscillators/GranularOsc.cpp
        ${SCALAR_DSP_ROOT}/gliese/Envelopes/ADSREnvelope.cpp
        ${SCALAR_DSP_ROOT}/gliese/Filter/DualFilter.cpp
        ${SCALAR_DSP_ROOT}/scalar/ScalarVoice.cpp
    INCLUDE_DIRS
        ${SCALAR_DSP_ROOT}/shim
        ${SCALAR_DSP_ROOT}
)

target_compile_options(${COMPONENT_LIB} PRIVATE -O2 -fno-fast-math)
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
