// Routes SWP messages from the watch (port of firmware/companion/main/dispatch.cpp).
#include <os/proc.h>
#include <sys/time.h>

#include <algorithm>

#include "app.hpp"

namespace dispatch {
namespace {

constexpr const char* kVersion = "ios-0.1";
bool ready = false;
uint32_t ticks = 0;
double last_hb = 0;

std::string device_name() { return cfg::get(cfg::kDeviceName, "SW iPhone"); }

swp::TimeSync time_sync() {
    swp::TimeSync t;
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    t.utc_ms = static_cast<uint64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    t.source = static_cast<uint8_t>(swp::TimeSource::Phone);
    t.posix_tz = cfg::get(cfg::kTimezone);
    return t;
}

swp::CompanionStatus companion_status() {
    swp::CompanionStatus s;
    s.firmware = kVersion;
    s.uptime_s = static_cast<uint32_t>(mono_s());
    s.sd_state = static_cast<uint8_t>(swp::SdState::Mounted);  // the app's data
    s.sd_total_mb = static_cast<uint32_t>(paths::total_bytes() >> 20);
    s.sd_free_mb = static_cast<uint32_t>(paths::free_bytes() >> 20);
    s.music_mb = static_cast<uint32_t>(navidrome::used_bytes() >> 20);
    s.maps_mb = static_cast<uint32_t>(cloud::maps_used() >> 20);
    auto ws = net::status();
    s.net_state = ws.state;
    s.net_provider = ws.provider;
    s.server_ok = net::server_ok();
    s.sync_state = static_cast<uint8_t>(navidrome::state());
    s.sync_done = navidrome::done();
    s.sync_total = navidrome::total();
    s.heap_free = static_cast<uint32_t>(std::min<size_t>(os_proc_available_memory(), UINT32_MAX));
    s.notif_pending = cloud::pending_notifications();
    s.last_notif_poll = ntfy::last_event_utc();
    s.internet_age_s = net::internet_age_s();
    return s;
}

void send_config_value(const cfg::KeyInfo& k, uint8_t status = 0) {
    swp::ConfigValue v;
    v.key = k.key;
    v.secret = k.secret;
    v.is_set = cfg::is_set(k.key);
    v.value = k.secret ? "" : cfg::get(k.key);  // secrets are write-only
    v.status = status;
    ble::send(v);
}

void send_error(swp::ErrorCode code, uint8_t ref, const char* msg) {
    swp::Error e;
    e.code = static_cast<uint8_t>(code);
    e.ref_type = ref;
    e.message = msg;
    ble::send(e);
}

void on_hello(const swp::Hello& h) {
    if (h.proto_version < swp::kProtocolVersion && h.proto_version < h.min_proto_version) {
        send_error(swp::ErrorCode::NotSupported, 0, "protocol version mismatch");
        return;
    }
    swp::HelloAck a;
    a.role = static_cast<uint8_t>(swp::Role::Phone);
    a.capabilities = swp::kCapTime | swp::kCapPosition | swp::kCapWeather | swp::kCapMaps | swp::kCapNotifications | swp::kCapMusic |
                     swp::kCapAudioDevices | swp::kCapConfig | swp::kCapTransfer | swp::kCapHttpProxy;
    a.firmware = kVersion;
    a.device_name = device_name();
    ble::send(a);
    ready = true;
    NSLog(@"hello from role %u fw %s", h.role, h.firmware.c_str());
    ble::send(time_sync());
    swp::Position p;
    if (position::current(p)) ble::send(p);
    swp::Weather w;
    if (cloud::weather(w)) ble::send(w);
    ble::send(player::status());
    ble::send(player::audio_status());
    ble::send(net::status());
    cloud::on_watch_connected();
}

void send_heartbeat() {
    swp::Heartbeat hb;
    hb.uptime_s = static_cast<uint32_t>(mono_s());
    hb.flags = 0;  // the phone has bandwidth for transfers during playback
    last_hb = mono_s();
    ble::send(hb);
}

void tick() {
    if (!ready) return;
    ++ticks;
    send_heartbeat();
    if (ticks % 3 == 0) {  // every minute
        swp::Position p;
        if (position::current(p)) ble::send(p);
        broadcast_status();
    }
    if (ticks % 1080 == 0) ble::send(time_sync());  // every 6 h
}

}  // namespace

void start() {
    [NSTimer scheduledTimerWithTimeInterval:20 repeats:YES block:^(NSTimer*) { tick(); }];
}

bool watch_ready() { return ready; }

void on_link_up() { ready = false; }

void on_link_down() {
    ready = false;
    cloud::on_watch_disconnected();
}

void broadcast_status() {
    if (!ready) return;
    ble::send(companion_status());
    ble::send(net::status());
}

void on_frame(const swp::Frame& f) {
    using swp::MsgType;
    auto type = static_cast<MsgType>(f.type);
    if (type != MsgType::Hello && !ready) {
        send_error(swp::ErrorCode::BadRequest, f.type, "send Hello first");
        return;
    }
    switch (type) {
        case MsgType::Hello:
            if (auto m = swp::decode_message<swp::Hello>(f)) on_hello(*m);
            break;
        case MsgType::Heartbeat:
            // A notification also wakes a suspended app: answer late heartbeats from here.
            if (mono_s() - last_hb > 15) send_heartbeat();
            break;
        case MsgType::TimeRequest:
            ble::send(time_sync());
            break;
        case MsgType::WeatherRequest:
            cloud::request_weather();
            break;
        case MsgType::MapTileRequest:
            if (auto m = swp::decode_message<swp::MapTileRequest>(f)) cloud::request_tile(*m);
            break;
        case MsgType::TransferAck:
            if (auto m = swp::decode_message<swp::TransferAck>(f)) cloud::on_transfer_ack(*m);
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
                cloud::request_search(*m);
            }
            break;
        case MsgType::RouteRequest:
            if (auto m = swp::decode_message<swp::RouteRequest>(f)) cloud::request_route(*m);
            break;
        case MsgType::MapDataCommand:
            if (auto m = swp::decode_message<swp::MapDataCommand>(f)) cloud::request_map_data(*m);
            break;
        case MsgType::HttpRequest:
            if (auto m = swp::decode_message<swp::HttpRequest>(f)) cloud::request_http(std::move(*m));
            break;
        case MsgType::Unpair:
            break;  // iOS owns its side of the bond; the watch's Forget deletes the watch's side
        case MsgType::NotificationAck:
            if (auto m = swp::decode_message<swp::NotificationAck>(f)) cloud::on_notification_ack(m->id);
            break;
        case MsgType::PlaylistsRequest: {
            swp::Playlists p;
            p.playlists = library::playlists();
            ble::send(p);
            break;
        }
        case MsgType::PlaylistRequest:
            if (auto m = swp::decode_message<swp::PlaylistRequest>(f)) ble::send(library::page(m->playlist_id, m->offset, m->count));
            break;
        case MsgType::PlaybackCommand:
            if (auto m = swp::decode_message<swp::PlaybackCommand>(f)) player::command(*m);
            break;
        case MsgType::MemoCommand:
            if (auto m = swp::decode_message<swp::MemoCommand>(f)) memos::command(*m);
            break;
        case MsgType::WifiConfigSet:
            send_error(swp::ErrorCode::NotSupported, f.type, "the phone manages its own network");
            ble::send(net::status());
            break;
        case MsgType::WifiStatusRequest:
            ble::send(net::status());
            break;
        case MsgType::AudioScanStart:
            ble::send(player::audio_devices());
            break;
        case MsgType::AudioConnect:
        case MsgType::AudioDisconnect:
        case MsgType::AudioForget:
            // TODO(PROTOTYPE): iOS has no API to change the audio route; Control Center does that.
            ble::send(player::audio_status());
            break;
        case MsgType::ConfigSet:
            if (auto m = swp::decode_message<swp::ConfigSet>(f)) {
                const cfg::KeyInfo* k = cfg::find(m->key);
                if (!k) {
                    swp::ConfigValue v;
                    v.key = m->key;
                    v.status = static_cast<uint8_t>(swp::ErrorCode::NotFound);
                    ble::send(v);
                    break;
                }
                bool ok = cfg::set(m->key, m->value);
                send_config_value(*k, ok ? 0 : static_cast<uint8_t>(swp::ErrorCode::BadRequest));
            }
            break;
        case MsgType::ConfigGet:
            if (auto m = swp::decode_message<swp::ConfigGet>(f)) {
                if (m->key.empty()) {
                    for (auto& k : cfg::keys()) send_config_value(k);
                } else if (const cfg::KeyInfo* k = cfg::find(m->key)) {
                    send_config_value(*k);
                }
            }
            break;
        case MsgType::WebConfigCommand: {
            swp::WebConfigStatus s;  // settings are in the app itself
            ble::send(s);
            break;
        }
        case MsgType::StatusRequest: {
            ble::send(companion_status());
            swp::DeviceInfo d;
            d.role = static_cast<uint8_t>(swp::Role::Phone);
            d.name = device_name();
            d.firmware = kVersion;
            d.hardware = "iPhone";
            ble::send(d);
            break;
        }
        default:
            send_error(swp::ErrorCode::NotSupported, f.type, "unsupported message");
    }
}

}  // namespace dispatch
