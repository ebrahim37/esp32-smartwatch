#include "ble/link.hpp"

#include <atomic>
#include <cstring>

#include "app/services.hpp"
#include "app/settings.hpp"
#include "app/state.hpp"
#include "app/tiles.hpp"
#include "ble/central.hpp"
#include "ble/peripheral.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "swp/transfer.hpp"

namespace swlink {
namespace {

constexpr const char* TAG = "link";
constexpr int kTxQueueLen = 48;
constexpr int64_t kHeartbeatUs = 20LL * 1000000;
constexpr int64_t kPeerSilentUs = 75LL * 1000000;  // > 3 missed heartbeats: drop and reconnect

struct TxItem {
    Target target;
    std::vector<uint8_t>* frame;
};

QueueHandle_t txq = nullptr;
std::atomic<uint16_t> seq{1};
std::atomic<bool> is_ready{false};
std::atomic<bool> web_up{false};
std::atomic<int64_t> last_rx_us{0};
std::atomic<bool> web_forward{false};  // web app asked for companion data through us

// Decoders and the transfer receiver are only touched from the NimBLE host task.
swp::FrameDecoder dec_companion;
swp::FrameDecoder dec_web;
swp::TransferReceiver tile_rx(256 * 1024, 8);
tiles::Key tile_rx_key{};

uint32_t own_caps() { return swp::kCapTime | swp::kCapWatchSettings | swp::kCapConfig; }

std::string fw_version() { return esp_app_get_description()->version; }

void tx_task(void*) {
    TxItem it;
    for (;;) {
        if (xQueueReceive(txq, &it, portMAX_DELAY) != pdTRUE) continue;
        std::vector<uint8_t>& f = *it.frame;
        bool ok = true;
        if (it.target == Target::Companion) {
            size_t chunk = central::mtu() > 3 ? central::mtu() - 3 : 20;
            for (size_t off = 0; ok && off < f.size(); off += chunk) ok = central::write(f.data() + off, std::min(chunk, f.size() - off));
        } else {
            size_t chunk = peripheral::mtu() > 3 ? peripheral::mtu() - 3 : 20;
            for (size_t off = 0; ok && off < f.size(); off += chunk) ok = peripheral::notify(f.data() + off, std::min(chunk, f.size() - off));
        }
        if (!ok) ESP_LOGD(TAG, "tx dropped (%u bytes)", (unsigned)f.size());
        delete it.frame;
    }
}

void send_hello() {
    swp::Hello h;
    h.role = static_cast<uint8_t>(swp::Role::Watch);
    h.capabilities = own_caps();
    h.firmware = fw_version();
    h.device_name = "SW Watch";
    send(h);
}

void after_handshake() {
    send(swp::TimeRequest{});
    send(swp::WifiStatusRequest{});
    send(swp::StatusRequest{});
    send(swp::WeatherRequest{});
    tiles::pump();
}

void handle_transfer_begin(const swp::TransferBegin& b) {
    swp::TransferAck ack;
    auto ev = tile_rx.on_begin(b, ack);
    tiles::Key k{};
    bool is_tile = b.kind == static_cast<uint8_t>(swp::TransferKind::MapTile) && tiles::decode_meta(b.meta, k);
    if (is_tile) tile_rx_key = k;
    send(ack);
    if (ev == swp::TransferReceiver::Event::Failed && is_tile) tiles::on_failed(k);
    if (ev == swp::TransferReceiver::Event::Complete && is_tile) tiles::on_tile(k, tile_rx.take_data());
}

void handle_transfer_chunk(const swp::TransferChunk& c) {
    swp::TransferAck ack;
    bool send_ack = false;
    auto ev = tile_rx.on_chunk(c, ack, send_ack);
    if (send_ack) send(ack);
    if (ev == swp::TransferReceiver::Event::Complete && tile_rx.kind() == static_cast<uint8_t>(swp::TransferKind::MapTile))
        tiles::on_tile(tile_rx_key, tile_rx.take_data());
}

void handle_notification(const swp::Notification& n) {
    auto st = settings::get();
    bool fresh = false;
    if (st.ntfy_enabled) {
        core::StoredNotification sn;
        sn.id = n.id;
        sn.timestamp = n.timestamp;
        sn.priority = n.priority;
        sn.title = n.title.empty() ? n.topic : n.title;
        sn.body = n.body;
        {
            app::Locked s;
            fresh = s->notifications.add(sn);
        }
        if (fresh) {
            app::mark(app::kDirtyNotifications);
            services::notifications_changed();
            app::Alert a;
            a.kind = app::Alert::Notification;
            a.title = sn.title;
            a.body = sn.body;
            app::post_alert(a);
            services::notify_feedback(n.priority >= 4 ? 3 : 1, false);
        }
    }
    // Ack even duplicates and disabled-notification drops, so the companion and server stop
    // redelivering (at-least-once semantics end to end).
    swp::NotificationAck ack;
    ack.id = n.id;
    send(ack);
}

void mirror_to_web(const swp::Frame& f) {
    if (web_up.load() && web_forward.load()) enqueue(Target::Web, f.type, std::vector<uint8_t>(f.payload));
}

void handle_companion(const swp::Frame& f) {
    using swp::MsgType;
    last_rx_us.store(esp_timer_get_time());
    switch (static_cast<MsgType>(f.type)) {
        case MsgType::HelloAck:
            if (auto m = swp::decode_message<swp::HelloAck>(f)) {
                is_ready.store(true);
                {
                    app::Locked s;
                    s->link.state = app::LinkState::Ready;
                    s->link.peer_firmware = m->firmware;
                    s->link.peer_caps = m->capabilities;
                    if (!m->device_name.empty()) s->link.peer_name = m->device_name;
                    s->link.last_error.clear();
                }
                app::mark(app::kDirtyLink);
                ESP_LOGI(TAG, "companion ready: %s fw %s caps 0x%08lx", m->device_name.c_str(), m->firmware.c_str(), (unsigned long)m->capabilities);
                after_handshake();
            }
            break;
        case MsgType::Heartbeat:
            if (auto m = swp::decode_message<swp::Heartbeat>(f)) {
                app::Locked s;
                s->link.peer_audio_active = m->flags & swp::kHeartbeatAudioActive;
            }
            break;
        case MsgType::TimeSync:
            if (auto m = swp::decode_message<swp::TimeSync>(f)) services::apply_time_sync(m->utc_ms);
            break;
        case MsgType::Position:
            if (auto m = swp::decode_message<swp::Position>(f)) {
                app::Locked s;
                s->position = *m;
                s->position_valid = true;
                app::mark(app::kDirtyPosition);
            }
            break;
        case MsgType::Weather:
            if (auto m = swp::decode_message<swp::Weather>(f)) {
                app::Locked s;
                s->weather = std::move(*m);
                s->weather_valid = true;
                app::mark(app::kDirtyWeather);
            }
            break;
        case MsgType::MapSearchResults:
            if (auto m = swp::decode_message<swp::MapSearchResults>(f)) {
                app::Locked s;
                s->search = std::move(*m);
                s->search_pending = false;
                app::mark(app::kDirtySearch);
            }
            break;
        case MsgType::MapSyncStatus:
            if (auto m = swp::decode_message<swp::MapSyncStatus>(f)) {
                app::Locked s;
                s->map_sync = std::move(*m);
                s->map_sync_valid = true;
                app::mark(app::kDirtyMapData);
            }
            break;
        case MsgType::MapDataStatus:
            if (auto m = swp::decode_message<swp::MapDataStatus>(f)) {
                app::Locked s;
                s->map_data = std::move(*m);
                s->map_data_valid = true;
                app::mark(app::kDirtyMapData);
            }
            break;
        case MsgType::Route:
            if (auto m = swp::decode_message<swp::Route>(f)) {
                app::Locked s;
                s->route = std::move(*m);
                s->route_pending = false;
                s->route_valid = s->route.status == 0 && s->route.points.size() >= 2;
                app::mark(app::kDirtyRoute);
            }
            break;
        case MsgType::TransferBegin:
            if (auto m = swp::decode_message<swp::TransferBegin>(f)) handle_transfer_begin(*m);
            break;
        case MsgType::TransferChunk:
            if (auto m = swp::decode_message<swp::TransferChunk>(f)) handle_transfer_chunk(*m);
            break;
        case MsgType::TransferAbort:
            if (auto m = swp::decode_message<swp::TransferAbort>(f)) {
                if (tile_rx.active() && tile_rx.id() == m->id) {
                    tile_rx.reset();
                    tiles::on_failed(tile_rx_key);
                }
            }
            break;
        case MsgType::Notification:
            if (auto m = swp::decode_message<swp::Notification>(f)) handle_notification(*m);
            break;
        case MsgType::Playlists:
            if (auto m = swp::decode_message<swp::Playlists>(f)) {
                app::Locked s;
                s->playlists = std::move(m->playlists);
                s->playlists_loaded = true;
                app::mark(app::kDirtyPlaylists);
            }
            break;
        case MsgType::PlaylistTracks:
            if (auto m = swp::decode_message<swp::PlaylistTracks>(f)) {
                app::Locked s;
                if (s->tracks_playlist != m->playlist_id || m->offset == 0) {
                    s->tracks.clear();
                    s->tracks_playlist = m->playlist_id;
                }
                s->tracks_total = m->total;
                if (m->offset == s->tracks.size())
                    for (auto& t : m->tracks) s->tracks.push_back(std::move(t));
                app::mark(app::kDirtyTracks);
                // Page in the rest automatically (small pages keep frames under the BLE MTU budget).
                if (s->tracks.size() < s->tracks_total && !m->tracks.empty())
                    request_tracks(m->playlist_id, static_cast<uint16_t>(s->tracks.size()));
            }
            break;
        case MsgType::PlaybackStatus:
            if (auto m = swp::decode_message<swp::PlaybackStatus>(f)) {
                app::Locked s;
                s->playback = std::move(*m);
                s->playback_rx_us = esp_timer_get_time();
                app::mark(app::kDirtyPlayback);
            }
            break;
        case MsgType::WifiStatus:
            if (auto m = swp::decode_message<swp::WifiStatus>(f)) {
                app::Locked s;
                s->wifi = *m;
                s->wifi_valid = true;
                app::mark(app::kDirtyWifi);
            }
            mirror_to_web(f);
            break;
        case MsgType::AudioDevices:
            if (auto m = swp::decode_message<swp::AudioDevices>(f)) {
                app::Locked s;
                s->audio_devices = std::move(*m);
                app::mark(app::kDirtyAudio);
            }
            break;
        case MsgType::AudioStatus:
            if (auto m = swp::decode_message<swp::AudioStatus>(f)) {
                app::Locked s;
                s->audio = *m;
                app::mark(app::kDirtyAudio);
            }
            break;
        case MsgType::CompanionStatus:
            if (auto m = swp::decode_message<swp::CompanionStatus>(f)) {
                app::Locked s;
                s->companion = *m;
                s->companion_valid = true;
                app::mark(app::kDirtyCompanion);
            }
            mirror_to_web(f);
            break;
        case MsgType::ConfigValue:
            if (auto m = swp::decode_message<swp::ConfigValue>(f)) {
                app::Locked s;
                bool replaced = false;
                for (auto& v : s->config_values)
                    if (v.key == m->key) {
                        v = *m;
                        replaced = true;
                    }
                if (!replaced && s->config_values.size() < 32) s->config_values.push_back(*m);
                app::mark(app::kDirtyConfig);
            }
            mirror_to_web(f);
            break;
        case MsgType::Error:
            if (auto m = swp::decode_message<swp::Error>(f)) {
                ESP_LOGW(TAG, "companion error %u for 0x%02x: %s", m->code, m->ref_type, m->message.c_str());
                if (m->ref_type == static_cast<uint8_t>(swp::MsgType::MapTileRequest)) {
                    unsigned z = 0, x = 0, y = 0;
                    if (sscanf(m->message.c_str(), "%u/%u/%u", &z, &x, &y) == 3) tiles::on_failed({static_cast<uint8_t>(z), x, y});
                    break;
                }
                app::Locked s;
                if (m->ref_type == static_cast<uint8_t>(swp::MsgType::MapSearchRequest)) s->search_pending = false;
                if (m->ref_type == static_cast<uint8_t>(swp::MsgType::RouteRequest)) s->route_pending = false;
                s->last_error_text = m->message;
                app::mark(app::kDirtySearch | app::kDirtyRoute | app::kDirtyCompanion);
            }
            mirror_to_web(f);
            break;
        default:
            ESP_LOGD(TAG, "ignored message 0x%02x", f.type);
            break;
    }
}

void send_device_info_web() {
    swp::DeviceInfo d;
    d.role = static_cast<uint8_t>(swp::Role::Watch);
    d.name = "SW Watch";
    d.firmware = fw_version();
    d.hardware = "Waveshare ESP32-S3-Touch-AMOLED-2.06";
    esp_read_mac(d.mac, ESP_MAC_BT);
    send_web(d);
}

void handle_web(const swp::Frame& f) {
    using swp::MsgType;
    switch (static_cast<MsgType>(f.type)) {
        case MsgType::Hello: {
            swp::HelloAck a;
            a.role = static_cast<uint8_t>(swp::Role::Watch);
            a.capabilities = own_caps();
            a.firmware = fw_version();
            a.device_name = "SW Watch";
            send_web(a);
            break;
        }
        case MsgType::StatusRequest:
            send_device_info_web();
            web_forward.store(true);
            if (ready()) send(swp::StatusRequest{});
            break;
        case MsgType::WatchSettingsRequest:
            send_web(settings::get());
            break;
        case MsgType::WatchSettings:
            if (auto m = swp::decode_message<swp::WatchSettings>(f)) {
                settings::set(*m);
                services::set_step_threshold_mg(m->step_threshold_mg);
                app::mark(app::kDirtySettings | app::kDirtyTime);
                send_web(settings::get());
            }
            break;
        case MsgType::WifiConfigSet:
        case MsgType::WifiStatusRequest:
        case MsgType::ConfigSet:
        case MsgType::ConfigGet:
            // Relay companion configuration through the watch's authenticated link.
            web_forward.store(true);
            if (ready()) {
                enqueue(Target::Companion, f.type, std::vector<uint8_t>(f.payload));
            } else {
                swp::Error e;
                e.code = static_cast<uint8_t>(swp::ErrorCode::Unavailable);
                e.ref_type = f.type;
                e.message = "companion not connected";
                send_web(e);
            }
            break;
        default: {
            swp::Error e;
            e.code = static_cast<uint8_t>(swp::ErrorCode::NotSupported);
            e.ref_type = f.type;
            e.message = "not supported by watch";
            send_web(e);
        }
    }
}

void health_cb(void*) {
    if (!is_ready.load()) return;
    int64_t now = esp_timer_get_time();
    if (now - last_rx_us.load() > kPeerSilentUs) {
        ESP_LOGW(TAG, "companion silent; dropping link");
        central::drop_link();
        return;
    }
    swp::Heartbeat hb;
    hb.uptime_s = static_cast<uint32_t>(now / 1000000);
    send(hb);
}

}  // namespace

void init() {
    txq = xQueueCreate(kTxQueueLen, sizeof(TxItem));
    xTaskCreatePinnedToCore(tx_task, "ble_tx", 4096, nullptr, 8, nullptr, 0);
    esp_timer_create_args_t a = {};
    a.callback = health_cb;
    a.name = "link_hb";
    esp_timer_handle_t t;
    esp_timer_create(&a, &t);
    esp_timer_start_periodic(t, kHeartbeatUs);
}

bool enqueue(Target t, uint8_t type, std::vector<uint8_t>&& payload) {
    if (t == Target::Companion && !central::connected()) return false;
    if (t == Target::Web && !web_up.load()) return false;
    auto* frame = new (std::nothrow) std::vector<uint8_t>();
    if (!frame) return false;
    if (!swp::encode_frame(type, seq.fetch_add(1), payload, *frame)) {
        delete frame;
        return false;
    }
    TxItem it{t, frame};
    if (xQueueSend(txq, &it, 0) != pdTRUE) {
        ESP_LOGW(TAG, "tx queue full; dropping 0x%02x", type);
        delete frame;
        return false;
    }
    return true;
}

bool ready() { return is_ready.load(); }

void on_companion_ready() {
    dec_companion.reset();
    last_rx_us.store(esp_timer_get_time());
    {
        app::Locked s;
        s->link.state = app::LinkState::Handshake;
    }
    app::mark(app::kDirtyLink);
    send_hello();
}

void on_companion_down() {
    is_ready.store(false);
    dec_companion.reset();
    tile_rx.reset();
    tiles::on_link_reset();
    {
        app::Locked s;
        s->search_pending = false;
        s->route_pending = false;
        s->link.peer_audio_active = false;
    }
    app::mark(app::kDirtyLink | app::kDirtySearch | app::kDirtyRoute);
}

void on_companion_bytes(const uint8_t* data, size_t len) {
    if (!dec_companion.feed(data, len)) ESP_LOGW(TAG, "rx overflow; resynchronising");
    swp::Frame f;
    swp::FrameDecoder::Result r;
    while ((r = dec_companion.next(f)) != swp::FrameDecoder::Result::NeedMore)
        if (r == swp::FrameDecoder::Result::Frame) handle_companion(f);
}

void on_web_connected() {
    dec_web.reset();
    web_up.store(true);
}

void on_web_down() {
    web_up.store(false);
    web_forward.store(false);
    dec_web.reset();
}

void on_web_bytes(const uint8_t* data, size_t len) {
    dec_web.feed(data, len);
    swp::Frame f;
    swp::FrameDecoder::Result r;
    while ((r = dec_web.next(f)) != swp::FrameDecoder::Result::NeedMore)
        if (r == swp::FrameDecoder::Result::Frame) handle_web(f);
}

void request_playlists() { send(swp::PlaylistsRequest{}); }

void request_tracks(const std::string& id, uint16_t offset) {
    swp::PlaylistRequest r;
    r.playlist_id = id;
    r.offset = offset;
    r.count = 12;
    send(r);
}

void playback(swp::PlaybackCmd cmd, uint32_t arg, const std::string& str) {
    swp::PlaybackCommand c;
    c.cmd = static_cast<uint8_t>(cmd);
    c.arg = arg;
    c.str = str;
    send(c);
}

void send_alert_tone() {
    bool sink = false;
    {
        app::Locked s;
        auto st = static_cast<swp::SinkState>(s->audio.state);
        sink = st == swp::SinkState::Connected || st == swp::SinkState::Streaming;
    }
    if (ready() && sink) playback(swp::PlaybackCmd::AlertTone, 3);
}

void request_status() {
    send(swp::StatusRequest{});
    send(swp::WifiStatusRequest{});
}

}  // namespace swlink
