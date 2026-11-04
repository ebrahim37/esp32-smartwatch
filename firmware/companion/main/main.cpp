// Companion firmware entry point (Freenove FNK0060 ESP32-WROVER).
//
// Boot order: NVS (system) -> cfg partition -> SD storage task -> Wi-Fi/connectivity ->
// Bluetooth (BLE GATT server + A2DP source) -> music player and sync -> cloud client.
#include <ctime>
#include <string>

#include "ble_server.hpp"
#include "cJSON.h"
#include "bt_core.hpp"
#include "cloud.hpp"
#include "ntfy.hpp"
#include "config.hpp"
#include "dispatch.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "music/memos.hpp"
#include "music/navidrome.hpp"
#include "music/player.hpp"
#include "net.hpp"
#include "nvs_flash.h"
#include "storage.hpp"
#include "web_config.hpp"

static const char* TAG = "main";

extern "C" void app_main(void) {
    // TODO(PROTOTYPE): report allocation failures (internal RAM is tight on this chip).
    heap_caps_register_failed_alloc_callback([](size_t size, uint32_t caps, const char* fn) {
        esp_rom_printf("ALLOC FAIL %u bytes caps 0x%x in %s (task %s), internal free %u largest %u\n", (unsigned)size, (unsigned)caps, fn,
                       pcTaskGetName(nullptr), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    });
    // JSON documents (Navidrome catalogue pages, library.json) become tens of thousands of
    // small nodes and key strings: keep them all in PSRAM, never in the scarce internal RAM.
    cJSON_Hooks hooks = {[](size_t n) { return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT); }, heap_caps_free};
    cJSON_InitHooks(&hooks);
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
    net::start();

    bt_core::init();
    dispatch::init();
    ble_server::init();
    music::player::init();  // also brings up A2DP/AVRCP
    music::memos::init();   // HFP Audio Gateway (headphone microphone)
    music::navidrome::start();
    cloud::start();
    ntfy::start();
    web_config::start();

    ESP_LOGI(TAG, "boot complete: internal free %u KB (min %u KB), PSRAM free %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024), (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    // Periodic status from an esp_timer, so app_main can return and free its 6 KB stack
    // (internal RAM is the companion's scarcest resource).
    static esp_timer_handle_t tick;
    esp_timer_create_args_t a = {};
    a.callback = [](void*) {
        static int n = 0;
        dispatch::broadcast_status();
        if (n++ % 10 == 0) {
            ESP_LOGI(TAG, "heap: internal %u KB (min %u KB), PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
                     (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
            // TODO(PROTOTYPE): stack high-water marks, to right-size task stacks.
            static TaskStatus_t ts[32];
            UBaseType_t cnt = uxTaskGetSystemState(ts, 32, nullptr);
            std::string line;
            for (UBaseType_t i = 0; i < cnt; ++i) line += std::string(ts[i].pcTaskName) + ":" + std::to_string(ts[i].usStackHighWaterMark) + " ";
            ESP_LOGI(TAG, "stack free (bytes): %s", line.c_str());
        }
    };
    a.name = "status";
    esp_timer_create(&a, &tick);
    esp_timer_start_periodic(tick, 30 * 1000 * 1000);
}
