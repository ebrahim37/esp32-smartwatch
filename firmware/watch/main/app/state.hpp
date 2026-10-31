// Central application state shared between the BLE/link tasks, background services and the
// LVGL UI task.
//
// Ownership rules:
//   * Producers (BLE host task, service tasks) modify State only while holding the lock
//     obtained through `app::Locked`, then call `app::mark(bits)`.
//   * The UI polls `app::take_dirty()` from an LVGL timer and refreshes the visible screen.
//     UI code reads State under the same lock and never blocks on Bluetooth.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/notification_store.hpp"
#include "swp/messages.hpp"

namespace app {

enum Dirty : uint32_t {
    kDirtyTime = 1u << 0,
    kDirtyLink = 1u << 1,
    kDirtyWeather = 1u << 2,
    kDirtyPosition = 1u << 3,
    kDirtyNotifications = 1u << 4,
    kDirtyPlaylists = 1u << 5,
    kDirtyTracks = 1u << 6,
    kDirtyPlayback = 1u << 7,
    kDirtyWifi = 1u << 8,
    kDirtyAudio = 1u << 9,
    kDirtyCompanion = 1u << 10,
    kDirtySearch = 1u << 11,
    kDirtyRoute = 1u << 12,
    kDirtyTiles = 1u << 13,
    kDirtyScan = 1u << 14,
    kDirtySensors = 1u << 15,
    kDirtyAlert = 1u << 16,
    kDirtyPasskey = 1u << 17,
    kDirtySettings = 1u << 18,
    kDirtyConfig = 1u << 19,
    kDirtyTimer = 1u << 20,
    kDirtyAlarms = 1u << 21,
    kDirtyWebConfig = 1u << 22,
    kDirtyMapData = 1u << 23,
};

enum class LinkState : uint8_t {
    NoPeer,        // never paired
    Disconnected,  // user pressed Disconnect: no automatic reconnection
    Waiting,       // backing off before the next reconnection attempt
    Connecting,
    Securing,      // encrypting / pairing
    Discovering,
    Handshake,     // GATT ready, waiting for HelloAck
    Ready,
};
const char* link_state_name(LinkState s);

struct LinkInfo {
    LinkState state = LinkState::NoPeer;
    bool has_peer = false;
    uint8_t peer_addr[6] = {};
    uint8_t peer_addr_type = 0;
    std::string peer_name;
    uint32_t retry_in_s = 0;
    uint32_t attempts = 0;
    uint16_t mtu = 23;
    std::string peer_firmware;
    uint32_t peer_caps = 0;
    std::string last_error;
    bool peer_audio_active = false;  // companion is streaming A2DP: avoid bulk requests
};

struct ScanEntry {
    uint8_t addr[6] = {};
    uint8_t addr_type = 0;
    std::string name;
    int8_t rssi = 0;
};

enum class PasskeyMode : uint8_t { None, EnterForCompanion, DisplayForWeb, ConfirmForWeb };

struct Alert {
    enum Kind : uint8_t { Timer, Alarm, Notification, Info } kind = Info;
    std::string title;
    std::string body;
    uint8_t alarm_id = 0;
};

struct State {
    LinkInfo link;
    std::vector<ScanEntry> scan;
    bool scanning = false;

    PasskeyMode passkey_mode = PasskeyMode::None;
    uint32_t passkey_value = 0;  // displayed / compared value

    bool weather_valid = false;
    swp::Weather weather;
    bool position_valid = false;
    swp::Position position;

    core::NotificationStore notifications;

    bool playlists_loaded = false;
    std::vector<swp::PlaylistInfo> playlists;
    std::string tracks_playlist;
    uint16_t tracks_total = 0;
    std::vector<swp::TrackInfo> tracks;
    swp::PlaybackStatus playback;
    int64_t playback_rx_us = 0;  // monotonic time the status arrived (for interpolation)

    bool wifi_valid = false;
    swp::WifiStatus wifi;
    swp::AudioStatus audio;
    swp::AudioDevices audio_devices;
    bool companion_valid = false;
    swp::CompanionStatus companion;
    std::vector<swp::ConfigValue> config_values;

    bool search_pending = false;
    swp::MapSearchResults search;
    bool route_pending = false;
    bool route_valid = false;
    swp::Route route;
    bool map_sync_valid = false;
    swp::MapSyncStatus map_sync;
    bool map_data_valid = false;
    swp::MapDataStatus map_data;

    std::vector<Alert> alerts;

    bool web_advertising = false;
    bool web_connected = false;
    std::string last_error_text;  // last Error from the companion (shown as a toast)
};

class Locked {
public:
    Locked();
    ~Locked();
    Locked(const Locked&) = delete;
    Locked& operator=(const Locked&) = delete;
    State* operator->() { return s_; }
    State& operator*() { return *s_; }

private:
    State* s_;
};

void init();
void mark(uint32_t bits);
uint32_t take_dirty();

// Posts an alert for the UI (vibration is handled by the caller).
void post_alert(const Alert& a);

}  // namespace app
