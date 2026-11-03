#include "music/a2dp.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "ble_server.hpp"
#include "esp_a2dp_api.h"
#include "esp_a2dp_legacy_api.h"
#include "esp_avrc_api.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "nvs.h"

namespace music::a2dp {
namespace {

constexpr const char* TAG = "a2dp";
std::mutex mu;
swp::AudioStatus st;
swp::AudioDevices devs;
uint8_t peer[6] = {};
bool have_peer = false;
std::string peer_name;
std::atomic<bool> is_connected{false};
std::atomic<bool> is_streaming{false};
// AVRCP play-status notification registered by the headphones (they only send button presses
// to a source that reports its playback status).
std::atomic<bool> play_status_registered{false};

void notify_play_status() {
    if (!play_status_registered.exchange(false)) return;  // AVRCP notifications are one-shot
    esp_avrc_rn_param_t rn = {};
    rn.playback = is_streaming.load() ? ESP_AVRC_PLAYBACK_PLAYING : ESP_AVRC_PLAYBACK_PAUSED;
    esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_PLAY_STATUS_CHANGE, ESP_AVRC_RN_RSP_CHANGED, &rn);
}
std::atomic<bool> media_ready{false};
std::atomic<bool> want_stream{false};
std::atomic<uint8_t> vol{80};
bool abs_vol_supported = false;
RemoteCmd remote = nullptr;
PcmSource source = nullptr;
// On-demand link: the companion connects when playback starts and lets go after a minute
// without audio, so headphones shared with a phone (AirPods) aren't taken away while the
// phone uses them. Nothing reconnects automatically.
constexpr uint32_t kIdleReleaseMs = 60 * 1000;
TimerHandle_t idle_timer = nullptr;

void publish() {
    swp::AudioStatus s;
    {
        std::lock_guard<std::mutex> g(mu);
        s = st;
    }
    ble_server::to_watch(s);
}

void set_state(swp::SinkState s) {
    {
        std::lock_guard<std::mutex> g(mu);
        st.state = static_cast<uint8_t>(s);
        memcpy(st.bda, peer, 6);
        st.name = peer_name;
        st.remembered = have_peer;
    }
    publish();
}

void save_peer() {
    nvs_handle_t h;
    if (nvs_open("a2dp", NVS_READWRITE, &h) != ESP_OK) return;
    if (have_peer) {
        nvs_set_blob(h, "peer", peer, 6);
        nvs_set_str(h, "name", peer_name.c_str());
    } else {
        nvs_erase_all(h);
    }
    nvs_commit(h);
    nvs_close(h);
}

void load_peer() {
    nvs_handle_t h;
    if (nvs_open("a2dp", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 6;
    have_peer = nvs_get_blob(h, "peer", peer, &len) == ESP_OK && len == 6;
    char name[64] = {};
    size_t nl = sizeof name;
    if (nvs_get_str(h, "name", name, &nl) == ESP_OK) peer_name = name;
    nvs_close(h);
}

void arm_idle_release() { xTimerReset(idle_timer, 0); }

void idle_cb(TimerHandle_t) {
    if (is_connected.load() && !is_streaming.load() && !want_stream.load()) {
        ESP_LOGI(TAG, "no audio for %u s; releasing %s", static_cast<unsigned>(kIdleReleaseMs / 1000), peer_name.c_str());
        set_state(swp::SinkState::Disconnecting);
        esp_a2d_source_disconnect(peer);
    }
}

void gap_cb(esp_bt_gap_cb_event_t ev, esp_bt_gap_cb_param_t* p) {
    switch (ev) {
        case ESP_BT_GAP_DISC_RES_EVT: {
            uint32_t cod = 0;
            int8_t rssi = -127;
            std::string name;
            for (int i = 0; i < p->disc_res.num_prop; ++i) {
                auto& pr = p->disc_res.prop[i];
                if (pr.type == ESP_BT_GAP_DEV_PROP_COD) cod = *static_cast<uint32_t*>(pr.val);
                else if (pr.type == ESP_BT_GAP_DEV_PROP_RSSI) rssi = *static_cast<int8_t*>(pr.val);
                else if (pr.type == ESP_BT_GAP_DEV_PROP_BDNAME) name.assign(static_cast<char*>(pr.val), strnlen(static_cast<char*>(pr.val), pr.len));
                else if (pr.type == ESP_BT_GAP_DEV_PROP_EIR && name.empty()) {
                    uint8_t l = 0;
                    uint8_t* n = esp_bt_gap_resolve_eir_data(static_cast<uint8_t*>(pr.val), ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &l);
                    if (!n) n = esp_bt_gap_resolve_eir_data(static_cast<uint8_t*>(pr.val), ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &l);
                    if (n) name.assign(reinterpret_cast<char*>(n), l);
                }
            }
            // Only rendering-capable audio devices (headphones, speakers).
            if (esp_bt_gap_get_cod_major_dev(cod) != ESP_BT_COD_MAJOR_DEV_AV) break;
            std::lock_guard<std::mutex> g(mu);
            for (auto& d : devs.devices)
                if (memcmp(d.bda, p->disc_res.bda, 6) == 0) {
                    if (!name.empty()) d.name = name;
                    d.rssi = rssi;
                    return;
                }
            if (devs.devices.size() >= 12) break;
            swp::AudioDevice d;
            memcpy(d.bda, p->disc_res.bda, 6);
            d.name = name.empty() ? "Audio device" : name;
            d.rssi = rssi;
            d.cod = cod;
            d.remembered = have_peer && memcmp(peer, d.bda, 6) == 0;
            devs.devices.push_back(d);
            break;
        }
        case ESP_BT_GAP_DISC_STATE_CHANGED_EVT: {
            bool scanning = p->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED;
            swp::AudioDevices copy;
            {
                std::lock_guard<std::mutex> g(mu);
                devs.scanning = scanning;
                copy = devs;
            }
            ble_server::to_watch(copy);
            if (!scanning && !is_connected.load()) set_state(swp::SinkState::Idle);
            break;
        }
        case ESP_BT_GAP_AUTH_CMPL_EVT:
            if (p->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
                std::lock_guard<std::mutex> g(mu);
                peer_name = reinterpret_cast<const char*>(p->auth_cmpl.device_name);
                ESP_LOGI(TAG, "paired with %s", peer_name.c_str());
            } else {
                ESP_LOGW(TAG, "pairing failed: %d", p->auth_cmpl.stat);
            }
            break;
        case ESP_BT_GAP_CFM_REQ_EVT:
            // No display/keyboard on the companion: confirm Secure Simple Pairing (the user
            // initiated this pairing from the watch).
            esp_bt_gap_ssp_confirm_reply(p->cfm_req.bda, true);
            break;
        default:
            break;
    }
}

void a2d_cb(esp_a2d_cb_event_t ev, esp_a2d_cb_param_t* p) {
    switch (ev) {
        case ESP_A2D_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "A2DP connection state %d", static_cast<int>(p->conn_stat.state));
            if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
                is_connected.store(true);
                memcpy(peer, p->conn_stat.remote_bda, 6);
                if (!have_peer) {
                    have_peer = true;
                    save_peer();
                } else {
                    save_peer();
                }
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                set_state(swp::SinkState::Connected);
                media_ready.store(false);
                esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
                arm_idle_release();
            } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
                is_connected.store(false);
                is_streaming.store(false);
                media_ready.store(false);
                xTimerStop(idle_timer, 0);
                set_state(swp::SinkState::Idle);  // e.g. the phone took the headphones: no reconnect
            } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
                set_state(swp::SinkState::Connecting);
            }
            break;
        case ESP_A2D_AUDIO_STATE_EVT:
            is_streaming.store(p->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED);
            ESP_LOGI(TAG, "audio %s", is_streaming.load() ? "started" : "suspended");
            notify_play_status();
            if (is_streaming.load()) xTimerStop(idle_timer, 0);
            else arm_idle_release();
            set_state(is_streaming.load() ? swp::SinkState::Streaming : swp::SinkState::Connected);
            break;
        case ESP_A2D_MEDIA_CTRL_ACK_EVT:
            ESP_LOGI(TAG, "media ctrl %d ack %d", static_cast<int>(p->media_ctrl_stat.cmd), static_cast<int>(p->media_ctrl_stat.status));
            if (p->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY && p->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
                media_ready.store(true);
                if (want_stream.load()) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
            }
            break;
        default:
            break;
    }
}

int32_t data_cb(uint8_t* buf, int32_t len) {
    if (!buf || len <= 0) return 0;
    if (source) return source(buf, len);
    memset(buf, 0, len);
    return len;
}

void tg_cb(esp_avrc_tg_cb_event_t ev, esp_avrc_tg_cb_param_t* p) {
    switch (ev) {
        case ESP_AVRC_TG_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "AVRCP target %s", p->conn_stat.connected ? "connected" : "disconnected");
            if (!p->conn_stat.connected) play_status_registered.store(false);
            break;
        case ESP_AVRC_TG_PASSTHROUGH_CMD_EVT:
            ESP_LOGI(TAG, "headphone button 0x%02x %s", p->psth_cmd.key_code, p->psth_cmd.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED ? "pressed" : "released");
            if (p->psth_cmd.key_state == ESP_AVRC_PT_CMD_STATE_PRESSED && remote) {
                switch (p->psth_cmd.key_code) {
                    case ESP_AVRC_PT_CMD_PLAY: remote(swp::PlaybackCmd::Resume); break;
                    case ESP_AVRC_PT_CMD_PAUSE: remote(swp::PlaybackCmd::Pause); break;
                    case ESP_AVRC_PT_CMD_STOP: remote(swp::PlaybackCmd::Pause); break;
                    case ESP_AVRC_PT_CMD_FORWARD: remote(swp::PlaybackCmd::Next); break;
                    case ESP_AVRC_PT_CMD_BACKWARD: remote(swp::PlaybackCmd::Previous); break;
                    default: break;
                }
            }
            break;
        case ESP_AVRC_TG_REMOTE_FEATURES_EVT:
            ESP_LOGI(TAG, "AVRCP controller features 0x%x", (unsigned)p->rmt_feats.feat_mask);
            break;
        case ESP_AVRC_TG_REGISTER_NOTIFICATION_EVT: {
            ESP_LOGI(TAG, "headphones registered for AVRCP event %u", p->reg_ntf.event_id);
            esp_avrc_rn_param_t rn = {};
            if (p->reg_ntf.event_id == ESP_AVRC_RN_PLAY_STATUS_CHANGE) {
                play_status_registered.store(true);
                rn.playback = is_streaming.load() ? ESP_AVRC_PLAYBACK_PLAYING : ESP_AVRC_PLAYBACK_PAUSED;
            } else if (p->reg_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                rn.volume = vol.load();
            } else {
                break;
            }
            esp_avrc_tg_send_rn_rsp(static_cast<esp_avrc_rn_event_ids_t>(p->reg_ntf.event_id), ESP_AVRC_RN_RSP_INTERIM, &rn);
            break;
        }
        case ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT:
            vol.store(p->set_abs_vol.volume);
            break;
        default:
            break;
    }
}

void ct_cb(esp_avrc_ct_cb_event_t ev, esp_avrc_ct_cb_param_t* p) {
    switch (ev) {
        case ESP_AVRC_CT_CONNECTION_STATE_EVT:
            if (p->conn_stat.connected) esp_avrc_ct_send_get_rn_capabilities_cmd(0);
            break;
        case ESP_AVRC_CT_GET_RN_CAPABILITIES_RSP_EVT:
            abs_vol_supported = esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_TEST, &p->get_rn_caps_rsp.evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
            ESP_LOGI(TAG, "headphones %s absolute volume (volume %u/127)", abs_vol_supported ? "support" : "don't support", vol.load());
            if (abs_vol_supported) {
                esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_VOLUME_CHANGE, 0);
                esp_avrc_ct_send_set_absolute_volume_cmd(2, vol.load());  // start at our volume, not wherever the phone left it
            }
            break;
        case ESP_AVRC_CT_CHANGE_NOTIFY_EVT:
            if (p->change_ntf.event_id == ESP_AVRC_RN_VOLUME_CHANGE) {
                vol.store(p->change_ntf.event_parameter.volume);
                esp_avrc_ct_send_register_notification_cmd(1, ESP_AVRC_RN_VOLUME_CHANGE, 0);
            }
            break;
        default:
            break;
    }
}

}  // namespace

void init(PcmSource src) {
    source = src;
    load_peer();
    idle_timer = xTimerCreate("a2dp_idle", pdMS_TO_TICKS(kIdleReleaseMs), pdFALSE, nullptr, idle_cb);
    esp_bt_gap_set_device_name("SW Companion");
    esp_bt_gap_register_callback(gap_cb);
    esp_bt_sp_param_t param = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param, &iocap, sizeof(uint8_t));

    esp_avrc_ct_init();
    esp_avrc_ct_register_callback(ct_cb);
    esp_avrc_tg_init();
    esp_avrc_tg_register_callback(tg_cb);
    esp_avrc_rn_evt_cap_mask_t evt_set = {};
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_VOLUME_CHANGE);
    esp_avrc_rn_evt_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &evt_set, ESP_AVRC_RN_PLAY_STATUS_CHANGE);
    esp_avrc_tg_set_rn_evt_cap(&evt_set);
    // Button presses the stack forwards to us (its default supported set is empty, so every
    // press would be answered "not implemented").
    esp_avrc_psth_bit_mask_t cmd_set = {};
    for (auto k : {ESP_AVRC_PT_CMD_PLAY, ESP_AVRC_PT_CMD_PAUSE, ESP_AVRC_PT_CMD_STOP, ESP_AVRC_PT_CMD_FORWARD, ESP_AVRC_PT_CMD_BACKWARD})
        esp_avrc_psth_bit_mask_operation(ESP_AVRC_BIT_MASK_OP_SET, &cmd_set, k);
    esp_avrc_tg_set_psth_cmd_filter(ESP_AVRC_PSTH_FILTER_SUPPORTED_CMD, &cmd_set);

    esp_a2d_register_callback(a2d_cb);
    esp_a2d_source_register_data_callback(data_cb);
    esp_a2d_source_init();
    // Not discoverable: the companion initiates all Classic connections.
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
    {
        std::lock_guard<std::mutex> g(mu);
        st.remembered = have_peer;
        memcpy(st.bda, peer, 6);
        st.name = peer_name;
    }
}

void scan(uint8_t seconds) {
    if (is_streaming.load()) return;  // inquiry would disrupt audio
    {
        std::lock_guard<std::mutex> g(mu);
        devs.devices.clear();
    }
    uint8_t len = static_cast<uint8_t>(std::clamp<int>(seconds * 100 / 128, 2, 20));  // units of 1.28 s
    set_state(swp::SinkState::Discovering);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, len, 0);
}

void connect(const uint8_t bda[6]) {
    esp_bt_gap_cancel_discovery();
    {
        std::lock_guard<std::mutex> g(mu);
        if (have_peer && memcmp(peer, bda, 6) != 0 && is_connected.load()) esp_a2d_source_disconnect(peer);
        memcpy(peer, bda, 6);
        for (auto& d : devs.devices)
            if (memcmp(d.bda, bda, 6) == 0) peer_name = d.name;
        have_peer = true;
    }
    set_state(swp::SinkState::Connecting);
    esp_a2d_source_connect(peer);
}

bool ensure_connected(uint32_t timeout_ms) {
    if (is_connected.load()) return true;
    if (!have_peer) {
        ESP_LOGW(TAG, "no remembered headphones; pair them under Settings > Connections");
        return false;
    }
    ESP_LOGI(TAG, "connecting to %s for playback", peer_name.c_str());
    set_state(swp::SinkState::Connecting);
    esp_a2d_source_connect(peer);
    for (uint32_t t = 0; t < timeout_ms && !is_connected.load(); t += 50) vTaskDelay(pdMS_TO_TICKS(50));
    if (!is_connected.load()) ESP_LOGW(TAG, "%s did not accept the connection within %u ms", peer_name.c_str(), static_cast<unsigned>(timeout_ms));
    return is_connected.load();
}

void disconnect() {
    xTimerStop(idle_timer, 0);
    if (is_connected.load()) {
        set_state(swp::SinkState::Disconnecting);
        esp_a2d_source_disconnect(peer);
    }
}

void forget(const uint8_t bda[6]) {
    if (have_peer && memcmp(peer, bda, 6) == 0) {
        disconnect();
        have_peer = false;
        peer_name.clear();
        save_peer();
    }
    esp_bt_gap_remove_bond_device(const_cast<uint8_t*>(bda));
    set_state(swp::SinkState::Idle);
}

bool connected() { return is_connected.load(); }
bool remote_volume() { return abs_vol_supported; }
bool streaming() { return is_streaming.load(); }

void start_stream() {
    want_stream.store(true);
    if (is_connected.load() && !is_streaming.load()) {
        if (media_ready.load()) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        else esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
    }
}

void suspend_stream() {
    want_stream.store(false);
    if (is_streaming.load()) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
    if (is_connected.load()) arm_idle_release();
}

void set_volume(uint8_t v) {
    vol.store(v > 127 ? 127 : v);
    if (abs_vol_supported) esp_avrc_ct_send_set_absolute_volume_cmd(2, vol.load());
}

uint8_t volume() { return vol.load(); }

swp::AudioStatus status() {
    std::lock_guard<std::mutex> g(mu);
    return st;
}

swp::AudioDevices devices() {
    std::lock_guard<std::mutex> g(mu);
    return devs;
}

void set_remote_handler(RemoteCmd h) { remote = h; }

}  // namespace music::a2dp
