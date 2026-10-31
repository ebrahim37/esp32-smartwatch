// Companion firmware entry point (Freenove FNK0060 ESP32-WROVER).
//
// Boot order: NVS (system) -> cfg partition -> SD storage task -> Wi-Fi/connectivity ->
// Bluetooth (BLE GATT server + A2DP source) -> music player and sync -> cloud client.
#include <ctime>

#include "ble_server.hpp"
#include "bt_core.hpp"
#include "cloud.hpp"
#include "ntfy.hpp"
#include "config.hpp"
#include "dispatch.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music/navidrome.hpp"
#include "music/player.hpp"
#include "net.hpp"
#include "nvs_flash.h"
#include "position.hpp"
#include "storage.hpp"

static const char* TAG = "main";

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "SW Companion %s starting", esp_app_get_description()->version);
    esp_err_t err = nvs_flash_init();  // PHY calibration, Bluetooth bonds, A2DP peer
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "system NVS layout changed; erasing (Bluetooth bonds will need re-pairing)");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    config::init();
    std::string tz = config::get(config::kTimezone);
    if (!tz.empty()) {
        setenv("TZ", tz.c_str(), 1);
        tzset();
    }

    storage::start();
    position::init();
    net::start();

    bt_core::init();
    dispatch::init();
    ble_server::init();
    music::player::init();  // also brings up A2DP/AVRCP
    music::navidrome::start();
    cloud::start();
    ntfy::start();

    ESP_LOGI(TAG, "boot complete: internal free %u KB, PSRAM free %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    for (int n = 0;; ++n) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        dispatch::broadcast_status();
        if (n % 10 == 0)
            ESP_LOGI(TAG, "heap: internal %u KB (min %u KB), PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
}
