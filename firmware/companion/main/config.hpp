// Companion configuration stored in the dedicated `cfg` NVS partition.
//
// Keys are whitelisted. Secret keys are write-only over BLE: they can be set but are never
// returned (ConfigValue only reports whether they are set). Nothing here is compiled into
// the firmware image; set them over BLE (watch Settings → Server / Wi-Fi, or the web app).
#pragma once

#include <string>
#include <vector>

namespace config {

struct KeyInfo {
    const char* key;
    bool secret;
    size_t max_len;
    const char* description;
};

// Whitelisted keys.
constexpr const char* kWifiSsid = "wifi_ssid";
constexpr const char* kWifiPass = "wifi_pass";
constexpr const char* kServerUrl = "srv_url";      // VPS helper base URL, https://...
constexpr const char* kServerToken = "srv_token";  // bearer token for the VPS helper
constexpr const char* kNdUrl = "nd_url";           // Navidrome base URL (through Caddy)
constexpr const char* kNdUser = "nd_user";
constexpr const char* kNdPass = "nd_pass";
constexpr const char* kNdHeader = "nd_hdr";        // name of the extra auth header for Caddy
constexpr const char* kNdHeaderValue = "nd_hdr_val";
// TODO(PROTOTYPE): exactly one playlist is synchronised; see CONFIG_PROTOTYPE_PLAYLIST_ID.
constexpr const char* kPrototypePlaylistIdKey = "nd_playlist";
// ntfy subscription (see ntfy.hpp). The token is sent as the reverse proxy's `Auth` header.
constexpr const char* kNtfyUrl = "ntfy_url";
constexpr const char* kNtfyToken = "ntfy_token";
constexpr const char* kNtfyTopics = "ntfy_topics";  // comma-separated
constexpr const char* kTimezone = "tz";
constexpr const char* kDeviceName = "dev_name";

const std::vector<KeyInfo>& keys();
const KeyInfo* find(const std::string& key);

void init();
std::string get(const char* key, const std::string& def = "");
bool is_set(const char* key);
// Validates against the whitelist and length limits; returns false on rejection.
bool set(const std::string& key, const std::string& value);

// Called after a successful set() so subsystems can apply changes without a reboot.
using Listener = void (*)(const std::string& key);
void add_listener(Listener l);

uint32_t ble_passkey();  // TODO(PROTOTYPE): fixed 616616 from Kconfig

}  // namespace config
