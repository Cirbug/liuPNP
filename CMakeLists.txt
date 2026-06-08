# ESP-IDF CMakeLists.txt — AI-PNP ESP32-P4 主控
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ai-pnp-p4)

# 源文件
set(SRCS src/main.c)

# 注册组件
idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS src
    REQUIRES
        freertos
        esp_system
        esp_wifi
        esp_camera
        esp_http_client
        nvs_flash
        nvs
        driver
        cJSON
        spi_flash
)
