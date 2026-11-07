cmake_minimum_required(VERSION 3.22)

# Scalar firmware — ESP32-S3 Eurorack full-voice module.
# Validated with ESP-IDF 5.2.3 (5.2.x profile):
#   idf.py set-target esp32s3
#   idf.py build flash monitor

list(APPEND EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/components)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(scalar_firmware)
