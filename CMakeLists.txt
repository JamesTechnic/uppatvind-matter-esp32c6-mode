cmake_minimum_required(VERSION 3.16)

# Ścieżka do esp-matter (zakładamy: ~/esp/esp-matter)
# Można nadpisać: cmake -DESP_MATTER_PATH=/inna/sciezka
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Ustaw ESP_MATTER_PATH: export ESP_MATTER_PATH=~/esp/esp-matter")
endif()

set(EXTRA_COMPONENT_DIRS
    $ENV{ESP_MATTER_PATH}/components
    $ENV{ESP_MATTER_PATH}/device_hal/device
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(uppatvind_matter)
