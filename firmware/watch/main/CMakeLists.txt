idf_component_register(
    SRC_DIRS . app ble ui ui/screens fonts
    INCLUDE_DIRS .
    REQUIRES board watch_core protocol bt nvs_flash esp_timer esp_app_format lvgl__lvgl esp_driver_gpio)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wno-missing-field-initializers)
