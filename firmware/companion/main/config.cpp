#include "config.hpp"

#include <mutex>

#include "ccore/ntfy.hpp"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace config {
namespace {

constexpr const char* TAG = "config";
constexpr const char* kPart = "cfg";
constexpr const char* kNs = "cfg";
std::mutex mu;
std::vector<Listener> listeners;

const std::vector<KeyInfo> kKeys = {
    {kWifiSsid, false, 32, "Home Wi-Fi SSID"},
    {kWifiPass, true, 64, "Home Wi-Fi password"},
    {kServerUrl, false, 128, "VPS helper base URL (https)"},
    {kServerToken, true, 128, "VPS helper device token"},
    {kNdUrl, false, 128, "Navidrome base URL (https)"},
    {kNdUser, false, 64, "Navidrome user"},
    {kNdPass, true, 128, "Navidrome password (used only for token auth)"},
    {kNdHeader, false, 64, "Extra auth header name for the reverse proxy"},
    {kNdHeaderValue, true, 256, "Extra auth header value"},
    {kPrototypePlaylistIdKey, false, 64, "PROTOTYPE_PLAYLIST_ID: playlist to sync"},
    {kNtfyUrl, false, 128, "ntfy base URL (https), e.g. https://ntfy-watch.example.com"},
    {kNtfyToken, true, 128, "ntfy reverse-proxy Auth header value"},
    {kNtfyTopics, false, 512, "ntfy topics to watch, comma-separated"},
    {kTimezone, false, 64, "POSIX TZ string"},
    {kDeviceName, false, 24, "BLE device name"},
};

bool valid_value(const KeyInfo& k, const std::string& v) {
    if (v.size() > k.max_len) return false;
    for (unsigned char c : v)
        if (c < 0x20 && c != '\t') return false;  // no control characters
    if ((std::string(k.key) == kServerUrl || std::string(k.key) == kNdUrl || std::string(k.key) == kNtfyUrl) && !v.empty() &&
        v.rfind("https://", 0) != 0)
        return false;  // TLS only
    if (std::string(k.key) == kNtfyTopics && !ccore::ntfy::valid_topic_list(v)) return false;
    return true;
}

}  // namespace

const std::vector<KeyInfo>& keys() { return kKeys; }

const KeyInfo* find(const std::string& key) {
    for (auto& k : kKeys)
        if (key == k.key) return &k;
    return nullptr;
}

void init() {
    esp_err_t err = nvs_flash_init_partition(kPart);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "cfg partition unreadable (%s); erasing", esp_err_to_name(err));
        nvs_flash_erase_partition(kPart);
        err = nvs_flash_init_partition(kPart);
    }
    ESP_ERROR_CHECK(err);
    if (get(kPrototypePlaylistIdKey).empty() && CONFIG_PROTOTYPE_PLAYLIST_ID[0]) set(kPrototypePlaylistIdKey, CONFIG_PROTOTYPE_PLAYLIST_ID);
}

std::string get(const char* key, const std::string& def) {
    std::lock_guard<std::mutex> g(mu);
    nvs_handle_t h;
    if (nvs_open_from_partition(kPart, kNs, NVS_READONLY, &h) != ESP_OK) return def;
    size_t len = 0;
    std::string out = def;
    if (nvs_get_str(h, key, nullptr, &len) == ESP_OK && len > 0 && len < 1024) {
        out.resize(len);
        if (nvs_get_str(h, key, out.data(), &len) == ESP_OK) out.resize(len - 1);
        else out = def;
    }
    nvs_close(h);
    return out;
}

bool is_set(const char* key) { return !get(key).empty(); }

bool set(const std::string& key, const std::string& value) {
    const KeyInfo* k = find(key);
    if (!k || !valid_value(*k, value)) {
        ESP_LOGW(TAG, "rejected config key '%s'", key.c_str());
        return false;
    }
    {
        std::lock_guard<std::mutex> g(mu);
        nvs_handle_t h;
        if (nvs_open_from_partition(kPart, kNs, NVS_READWRITE, &h) != ESP_OK) return false;
        esp_err_t err = value.empty() ? nvs_erase_key(h, k->key) : nvs_set_str(h, k->key, value.c_str());
        if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
        if (err == ESP_OK) err = nvs_commit(h);
        nvs_close(h);
        if (err != ESP_OK) return false;
    }
    ESP_LOGI(TAG, "updated %s%s", k->key, k->secret ? " (secret)" : "");
    for (auto l : listeners) l(key);
    return true;
}

void add_listener(Listener l) { listeners.push_back(l); }

// TODO(PROTOTYPE): fixed passkey (616 616); production needs a per-device random one.
uint32_t ble_passkey() { return static_cast<uint32_t>(CONFIG_COMPANION_PROTOTYPE_PASSKEY); }

}  // namespace config
