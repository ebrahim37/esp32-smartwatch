idf_component_register(
    SRC_DIRS . music
    INCLUDE_DIRS .
    REQUIRES bt esp_wifi esp_netif esp_event nvs_flash esp_http_client esp-tls fatfs sdmmc esp_driver_sdmmc esp_driver_gpio
             esp_timer esp_app_format json mbedtls protocol companion_core espressif__esp_audio_codec)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wno-missing-field-initializers)
