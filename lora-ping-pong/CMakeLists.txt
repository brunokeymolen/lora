cmake_minimum_required(VERSION 3.16)

# ra01s SX126x driver vendored into components/ra01s (patched from nopnop2002/esp-idf-sx126x)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lora-ping-pong)
