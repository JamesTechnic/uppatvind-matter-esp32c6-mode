cmake_minimum_required(VERSION 3.16)

# Ścieżka do esp-matter (zakładamy: ~/esp/esp-matter)
# Można nadpisać: cmake -DESP_MATTER_PATH=/inna/sciezka
if(NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Ustaw ESP_MATTER_PATH: export ESP_MATTER_PATH=~/esp/esp-matter")
endif()

set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)

# This should be done before using the IDF_TARGET variable.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

set(EXTRA_COMPONENT_DIRS
    $ENV{ESP_MATTER_PATH}/components
    "${MATTER_SDK_PATH}/config/esp32/components"
    ${extra_components_dirs_append}    
)

project(uppatvind_matter)

idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-DCHIP_HAVE_CONFIG_H;-Wno-overloaded-virtual;-Wno-error=unused-result" APPEND)
idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security;-Wformat=0" APPEND)
