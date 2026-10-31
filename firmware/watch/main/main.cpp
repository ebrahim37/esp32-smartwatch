// Watch firmware entry point.
//
// Boot order: NVS -> settings -> board (I2C, PMIC, display/LVGL, touch, IMU, motor) ->
// state -> UI -> services -> BLE link + central/peripheral.
#include "app/services.hpp"
#include "app/settings.hpp"
#include "app/state.hpp"
#include "app/tiles.hpp"
#include "ble/central.hpp"
#include "ble/link.hpp"
#include "board.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "ui/ui.hpp"

static const char* TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "SW Watch %s starting", esp_app_get_description()->version);

    // Default NVS partition: PHY calibration and NimBLE bonds.
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "system NVS layout changed; erasing (bonds will need re-pairing)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    settings::init();
    app::init();

    ESP_ERROR_CHECK(board::init());
    tiles::init();

    if (board::lvgl_lock(1000)) {
        ui::init();
        board::lvgl_unlock();
    }

    services::start();
    swlink::init();
    central::init();

    ESP_LOGI(TAG, "boot complete: internal free %u KB, PSRAM free %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        ESP_LOGI(TAG, "heap: internal %u KB (min %u KB), PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}
