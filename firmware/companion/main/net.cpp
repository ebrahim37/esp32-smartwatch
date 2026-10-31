#include "net.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "ccore/backoff.hpp"
#include "config.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lte.hpp"

namespace net {
namespace {

constexpr const char* TAG = "net";
constexpr EventBits_t kGotIp = 1 << 0;
constexpr EventBits_t kDisconnected = 1 << 1;
constexpr EventBits_t kScanDone = 1 << 2;
constexpr EventBits_t kReconfigure = 1 << 3;

EventGroupHandle_t events;
std::mutex mu;
swp::WifiStatus st;
std::atomic<bool> is_online{false};
std::atomic<bool> synced{false};
std::vector<Listener> listeners;
esp_netif_t* sta = nullptr;
ccore::Backoff scan_backoff(10000, 300000, 20);

void set_state(swp::NetState s) {
    std::lock_guard<std::mutex> g(mu);
    st.state = static_cast<uint8_t>(s);
}

void notify(bool online) {
    for (auto l : listeners) l(online);
}

void on_event(void*, esp_event_base_t base, int32_t id, void* data) {
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        auto* d = static_cast<wifi_event_sta_disconnected_t*>(data);
        ESP_LOGI(TAG, "Wi-Fi disconnected (reason %d)", d->reason);
        xEventGroupSetBits(events, kDisconnected);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(events, kScanDone);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto* e = static_cast<ip_event_got_ip_t*>(data);
        {
            std::lock_guard<std::mutex> g(mu);
            st.ipv4 = e->ip_info.ip.addr;
        }
        xEventGroupSetBits(events, kGotIp);
    }
}

void time_sync_cb(struct timeval*) {
    if (!synced.exchange(true)) ESP_LOGI(TAG, "SNTP time synchronised");
}

bool scan_for(const std::string& ssid) {
    wifi_scan_config_t sc = {};
    sc.ssid = reinterpret_cast<uint8_t*>(const_cast<char*>(ssid.c_str()));
    sc.show_hidden = true;
    sc.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    sc.scan_time.active.min = 60;
    sc.scan_time.active.max = 120;
    xEventGroupClearBits(events, kScanDone);
    if (esp_wifi_scan_start(&sc, false) != ESP_OK) return false;
    if (!(xEventGroupWaitBits(events, kScanDone, pdTRUE, pdFALSE, pdMS_TO_TICKS(8000)) & kScanDone)) {
        esp_wifi_scan_stop();
        return false;
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    bool found = n > 0;
    esp_wifi_clear_ap_list();
    return found;
}

bool connect(const std::string& ssid, const std::string& pass) {
    wifi_config_t wc = {};
    strlcpy(reinterpret_cast<char*>(wc.sta.ssid), ssid.c_str(), sizeof wc.sta.ssid);
    strlcpy(reinterpret_cast<char*>(wc.sta.password), pass.c_str(), sizeof wc.sta.password);
    wc.sta.threshold.authmode = pass.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_wifi_set_config(WIFI_IF_STA, &wc);
    set_state(swp::NetState::Connecting);
    xEventGroupClearBits(events, kGotIp | kDisconnected);
    if (esp_wifi_connect() != ESP_OK) return false;
    EventBits_t b = xEventGroupWaitBits(events, kGotIp | kDisconnected, pdFALSE, pdFALSE, pdMS_TO_TICKS(20000));
    if (b & kGotIp) return true;
    esp_wifi_disconnect();
    return false;
}

void manager_task(void*) {
    for (;;) {
        std::string ssid = config::get(config::kWifiSsid);
        std::string pass = config::get(config::kWifiPass);
        {
            std::lock_guard<std::mutex> g(mu);
            st.ssid = ssid;
            st.password_set = !pass.empty();
        }
        if (ssid.empty()) {
            set_state(swp::NetState::NotConfigured);
            xEventGroupWaitBits(events, kReconfigure, pdTRUE, pdFALSE, portMAX_DELAY);
            continue;
        }
        set_state(swp::NetState::Searching);
        bool found = scan_for(ssid);
        if (found && connect(ssid, pass)) {
            scan_backoff.reset();
            lte::stop();  // never carry data over LTE while Wi-Fi is up
            wifi_ap_record_t ap;
            if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
                std::lock_guard<std::mutex> g(mu);
                st.rssi = ap.rssi;
            }
            {
                std::lock_guard<std::mutex> g(mu);
                st.state = static_cast<uint8_t>(swp::NetState::Connected);
                st.provider = static_cast<uint8_t>(swp::NetProvider::Wifi);
            }
            is_online.store(true);
            ESP_LOGI(TAG, "online via Wi-Fi '%s'", ssid.c_str());
            notify(true);
            // Stay here until the link drops or the configuration changes.
            EventBits_t b = xEventGroupWaitBits(events, kDisconnected | kReconfigure, pdTRUE, pdFALSE, portMAX_DELAY);
            is_online.store(false);
            {
                std::lock_guard<std::mutex> g(mu);
                st.provider = static_cast<uint8_t>(swp::NetProvider::None);
                st.ipv4 = 0;
                st.internet_ok = 0;
            }
            notify(false);
            if (b & kReconfigure) esp_wifi_disconnect();
            continue;  // retry immediately once, then back off
        }
        // TODO(LTE): fall back to the cellular provider here when lte::present().
        if (lte::present()) lte::start();
        set_state(found ? swp::NetState::Failed : swp::NetState::Searching);
        uint32_t delay = scan_backoff.next(esp_random());
        ESP_LOGI(TAG, "'%s' %s; next attempt in %lu s", ssid.c_str(), found ? "connect failed" : "not in range", (unsigned long)(delay / 1000));
        xEventGroupWaitBits(events, kReconfigure, pdTRUE, pdFALSE, pdMS_TO_TICKS(delay));
    }
}

void on_config(const std::string& key) {
    if (key == config::kWifiSsid || key == config::kWifiPass) reconfigure();
}

}  // namespace

void start() {
    events = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta = esp_netif_create_default_wifi_sta();
    std::string name = config::get(config::kDeviceName, "sw-companion");
    esp_netif_set_hostname(sta, name.c_str());
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, nullptr);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, nullptr);
    esp_wifi_set_storage(WIFI_STORAGE_RAM);  // credentials live in the cfg partition only
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_ps(WIFI_PS_MIN_MODEM);  // required for Wi-Fi/Bluetooth coexistence

    esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("time.cloudflare.com");
    sc.sync_cb = time_sync_cb;
    sc.start = true;
    esp_netif_sntp_init(&sc);

    config::add_listener(on_config);
    xTaskCreatePinnedToCore(manager_task, "net", 4096, nullptr, 4, nullptr, 0);
}

swp::WifiStatus status() {
    std::lock_guard<std::mutex> g(mu);
    if (st.state == static_cast<uint8_t>(swp::NetState::Connected)) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) st.rssi = ap.rssi;
    }
    return st;
}

bool online() { return is_online.load(); }
swp::NetProvider provider() { return online() ? swp::NetProvider::Wifi : swp::NetProvider::None; }
void reconfigure() { xEventGroupSetBits(events, kReconfigure); }
bool time_synced() { return synced.load(); }

void set_server_ok(bool ok) {
    std::lock_guard<std::mutex> g(mu);
    st.internet_ok = ok;
}

void add_listener(Listener l) { listeners.push_back(l); }

}  // namespace net
