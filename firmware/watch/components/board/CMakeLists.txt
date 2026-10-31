# Board abstraction for the Waveshare ESP32-S3-Touch-AMOLED-2.06 prototype.
# Application code only sees include/board.hpp; swapping in the custom watch PCB means
# providing another implementation of that interface.
idf_component_register(
    SRCS waveshare_amoled_206.cpp axp2101.cpp lv_mem_psram.c
    INCLUDE_DIRS include
    REQUIRES lvgl__lvgl esp_driver_i2c esp_driver_gpio esp_timer
    PRIV_REQUIRES waveshare__esp32_s3_touch_amoled_2_06 waveshare__qmi8658 heap esp_lvgl_port)

# LVGL (CONFIG_LV_USE_CUSTOM_MALLOC) calls lv_malloc_core() etc. defined here; make the
# static-link order resolve them.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC ${COMPONENT_LIB})
