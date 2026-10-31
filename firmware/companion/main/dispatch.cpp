#include "dispatch.hpp"

#include <sys/time.h>

#include "ble_server.hpp"
#include "cloud.hpp"
#include "config.hpp"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "music/a2dp.hpp"
#include "music/library.hpp"
#include "music/navidrome.hpp"
#include "music/player.hpp"
#include "net.hpp"
#include "ntfy.hpp"
#include "position.hpp"
#include "storage.hpp"
#include "swp/messages.hpp"

namespace dispatch {
namespace {

constexpr const char* TAG = "dispatch";
esp_timer_handle_t tick_timer;

uint32_t caps() {
    return swp::kCapTime | swp::kCapPosition | swp::kCapWeather | swp::kCapMaps | swp::kCapNotifications | swp::kCapMusic |
           swp::kCapWifiConfig | swp::kCapAudioDevices | swp::kCapConfig | swp::kCapTransfer;
}

swp::TimeSync time_sync() {
    swp::TimeSync t;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    if (tv.tv_sec > 1704067200) {  // only send a wall clock we believe in
        t.utc_ms = static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
        t.source = static_cast<uint8_t>(net::time_synced() ? swp::TimeSource::Ntp : swp::TimeSource::None);
    }
    t.posix_tz = config::get(config::kTimezone);
    return t;
}

swp::CompanionStatus companion_status() {
    swp::CompanionStatus s;
    s.firmware = esp_app_get_description()->version;
    s.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    auto si = storage::info();
    s.sd_state = static_cast<uint8_t>(si.state);
    s.sd_total_mb = static_cast<uint32_t>(si.total >> 20);
    s.sd_free_mb = static_cast<uint32_t>(si.free >> 20);
    s.music_mb = static_cast<uint32_t>(si.music_used >> 20);
    s.maps_mb = static_cast<uint32_t>(si.maps_used >> 20);
    auto ws = net::status();
    s.net_state = ws.state;
    s.net_provider = ws.provider;
    s.server_ok = cloud::server_ok();
    s.sync_state = static_cast<uint8_t>(music::navidrome::state());
    s.sync_done = music::navidrome::done();
    s.sync_total = music::navidrome::total();
    s.heap_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    s.psram_free = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s.notif_pending = cloud::pending_notifications();
    s.last_notif_poll = ntfy::last_event_utc();
    return s;
}

void send_config_value(uint16_t conn, const config::KeyInfo& k, uint8_t status = 0) {
    swp::ConfigValue v;
    v.key = k.key;
    v.secret = k.secret;
    v.is_set = config::is_set(k.key);
    v.value = k.secret ? "" : config::get(k.key);  // secrets are write-only
    v.status = status;
    ble_server::send(conn, v);
}

void on_hello(uint16_t conn, const swp::Hello& h) {
    if (h.proto_version < swp::kProtocolVersion && h.proto_version < h.min_proto_version) {
        swp::Error e;
        e.code = static_cast<uint8_t>(swp::ErrorCode::NotSupported);
        e.message = "protocol version mismatch";
        ble_server::send(conn, e);
        return;
    }
    ble_server::set_role(conn, h.role);
    swp::HelloAck a;
    a.role = static_cast<uint8_t>(swp::Role::Companion);
    a.capabilities = caps();
    a.firmware = esp_app_get_description()->version;
    a.device_name = config::get(config::kDeviceName, "SW Companion");
    ble_server::send(conn, a);
    ESP_LOGI(TAG, "hello from role %u fw %s (%s)", h.role, h.firmware.c_str(), h.device_name.c_str());
    if (h.role == static_cast<uint8_t>(swp::Role::Watch)) {
        // TODO(PHONE): a phone app session would get the same initial state push.
        ble_server::send(conn, time_sync());
        swp::Position p;
        if (position::current(p)) ble_server::send(conn, p);
        swp::Weather w;
        if (cloud::weather(w)) ble_server::send(conn, w);
        ble_server::send(conn, music::player::status());
        ble_server::send(conn, music::a2dp::status());
        ble_server::send(conn, net::status());
        cloud::on_watch_connected();
    }
}

void tick(void*) {
    if (!ble_server::watch_connected()) return;
    static uint32_t n = 0;
    ++n;
    swp::Heartbeat hb;
    hb.uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
    hb.flags = music::player::streaming() ? swp::kHeartbeatAudioActive : 0;
    ble_server::to_watch(hb);
    if (n % 3 == 0) {  // every minute
        swp::Position p;
        if (position::current(p)) ble_server::to_watch(p);
    }
    // Push time as soon as SNTP first succeeds, then every 6 h.
    static bool sent_synced = false;
    if ((net::time_synced() && !sent_synced) || n % 1080 == 0) {
        sent_synced = net::time_synced();
        ble_server::to_watch(time_sync());
    }
}

}  // namespace

void init() {
    esp_timer_create_args_t a = {};
    a.callback = tick;
    a.name = "dispatch";
    esp_timer_create(&a, &tick_timer);
    esp_timer_start_periodic(tick_timer, 20LL * 1000000);
}

void broadcast_status() {
    ble_server::to_watch(companion_status());
    ble_server::to_watch(net::status());
}

void on_disconnect(uint16_t conn, uint8_t role) {
    (void)conn;
    if (role == static_cast<uint8_t>(swp::Role::Watch)) cloud::on_watch_disconnected();
}

void on_frame(uint16_t conn, const swp::Frame& f) {
    using swp::MsgType;
    auto type = static_cast<MsgType>(f.type);
    if (type != MsgType::Hello && ble_server::role(conn) == 0) {
        swp::Error e;
        e.code = static_cast<uint8_t>(swp::ErrorCode::BadRequest);
        e.ref_type = f.type;
        e.message = "send Hello first";
        ble_server::send(conn, e);
        return;
    }
    switch (type) {
        case MsgType::Hello:
            if (auto m = swp::decode_message<swp::Hello>(f)) on_hello(conn, *m);
            break;
        case MsgType::Heartbeat:
            break;
        case MsgType::TimeRequest:
            ble_server::send(conn, time_sync());
            break;
        case MsgType::WeatherRequest:
            cloud::request_weather(conn);
            break;
        case MsgType::MapTileRequest:
            if (auto m = swp::decode_message<swp::MapTileRequest>(f)) cloud::request_tile(conn, *m);
            break;
        case MsgType::TransferAck:
            if (auto m = swp::decode_message<swp::TransferAck>(f)) cloud::on_transfer_ack(conn, *m);
            break;
        case MsgType::MapSearchRequest:
            if (auto m = swp::decode_message<swp::MapSearchRequest>(f)) {
                if (m->lat_e7 == 0 && m->lon_e7 == 0) {
                    swp::Position p;
                    if (position::current(p)) {
                        m->lat_e7 = p.lat_e7;
                        m->lon_e7 = p.lon_e7;
                    }
                }
                cloud::request_search(conn, *m);
            }
            break;
        case MsgType::RouteRequest:
            if (auto m = swp::decode_message<swp::RouteRequest>(f)) cloud::request_route(conn, *m);
            break;
        case MsgType::MapSyncRequest:
            if (auto m = swp::decode_message<swp::MapSyncRequest>(f)) cloud::request_map_sync(conn, *m);
            break;
        case MsgType::MapDataCommand:
            if (auto m = swp::decode_message<swp::MapDataCommand>(f)) cloud::request_map_data(conn, *m);
            break;
        case MsgType::NotificationAck:
            if (auto m = swp::decode_message<swp::NotificationAck>(f)) cloud::on_notification_ack(m->id);
            break;
        case MsgType::PlaylistsRequest: {
            swp::Playlists p;
            p.playlists = music::library::playlists();
            ble_server::send(conn, p);
            break;
        }
        case MsgType::PlaylistRequest:
            if (auto m = swp::decode_message<swp::PlaylistRequest>(f)) ble_server::send(conn, music::library::page(m->playlist_id, m->offset, m->count));
            break;
        case MsgType::PlaybackCommand:
            if (auto m = swp::decode_message<swp::PlaybackCommand>(f)) music::player::command(*m);
            break;
        case MsgType::WifiConfigSet:
            if (auto m = swp::decode_message<swp::WifiConfigSet>(f)) {
                bool ok = config::set(config::kWifiSsid, m->ssid) && config::set(config::kWifiPass, m->password);
                if (!ok) {
                    swp::Error e;
                    e.code = static_cast<uint8_t>(swp::ErrorCode::BadRequest);
                    e.ref_type = f.type;
                    e.message = "invalid Wi-Fi settings";
                    ble_server::send(conn, e);
                }
                ble_server::send(conn, net::status());
            }
            break;
        case MsgType::WifiStatusRequest:
            ble_server::send(conn, net::status());
            break;
        case MsgType::AudioScanStart:
            if (auto m = swp::decode_message<swp::AudioScanStart>(f)) music::a2dp::scan(m->seconds);
            break;
        case MsgType::AudioConnect:
            if (auto m = swp::decode_message<swp::AudioConnect>(f)) music::a2dp::connect(m->bda);
            break;
        case MsgType::AudioDisconnect:
            music::a2dp::disconnect();
            break;
        case MsgType::AudioForget:
            if (auto m = swp::decode_message<swp::AudioForget>(f)) music::a2dp::forget(m->bda);
            break;
        case MsgType::ConfigSet:
            if (auto m = swp::decode_message<swp::ConfigSet>(f)) {
                const config::KeyInfo* k = config::find(m->key);
                if (!k) {
                    swp::ConfigValue v;
                    v.key = m->key;
                    v.status = static_cast<uint8_t>(swp::ErrorCode::NotFound);
                    ble_server::send(conn, v);
                    break;
                }
                bool ok = config::set(m->key, m->value);
                send_config_value(conn, *k, ok ? 0 : static_cast<uint8_t>(swp::ErrorCode::BadRequest));
            }
            break;
        case MsgType::ConfigGet:
            if (auto m = swp::decode_message<swp::ConfigGet>(f)) {
                if (m->key.empty()) {
                    for (auto& k : config::keys()) send_config_value(conn, k);
                } else if (const config::KeyInfo* k = config::find(m->key)) {
                    send_config_value(conn, *k);
                }
            }
            break;
        case MsgType::StatusRequest: {
            ble_server::send(conn, companion_status());
            swp::DeviceInfo d;
            d.role = static_cast<uint8_t>(swp::Role::Companion);
            d.name = config::get(config::kDeviceName, "SW Companion");
            d.firmware = esp_app_get_description()->version;
            d.hardware = "Freenove FNK0060 ESP32-WROVER";
            esp_read_mac(d.mac, ESP_MAC_BT);
            ble_server::send(conn, d);
            break;
        }
        default: {
            swp::Error e;
            e.code = static_cast<uint8_t>(swp::ErrorCode::NotSupported);
            e.ref_type = f.type;
            e.message = "unsupported message";
            ble_server::send(conn, e);
        }
    }
}

}  // namespace dispatch
