# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${PROJECT_NAME})

# 源文件
file(GLOB_RECURSE SRCS src/main.c)

# 主程序
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

# cJSON 是 ESP-IDF 内置组件, 不需额外下载

# 如果需要挂载 SD 卡/FATFS, 取消下面注释:
# set(EXTRA_COMPONENT_DIRS $ENV{IDF_PATH}/examples/storage/sd_card/sdmmc/components)
