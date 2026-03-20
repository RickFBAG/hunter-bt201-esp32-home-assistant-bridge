FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    INCLUDE_DIRS "../include"
    REQUIRES bt mqtt nvs_flash esp_wifi esp_event esp_timer esp_netif json driver esp_lcd esp_lcd_sh8601
)
