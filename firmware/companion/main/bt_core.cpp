#include "bt_core.hpp"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_log.h"

namespace bt_core {

void init() {
    esp_bt_controller_config_t cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BTDM));  // BLE + BR/EDR
    esp_bluedroid_config_t bcfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    bcfg.ssp_en = true;
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bcfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());
    ESP_LOGI("bt", "Bluetooth dual-mode stack up");
}

}  // namespace bt_core
