cmake_minimum_required(VERSION 3.16)

# ra01s SX126x driver and ssd1306 vendored into components/ (patched from lora-ping-pong)
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_SOURCE_DIR}/components"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lora-text)
