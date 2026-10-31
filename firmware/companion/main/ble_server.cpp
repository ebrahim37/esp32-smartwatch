#include "ble_server.hpp"

#include <atomic>
#include <cstring>
#include <mutex>

#include "config.hpp"
#include "dispatch.hpp"
#include "driver/gpio.h"
#include "esp_app_desc.h"
#include "esp_bt_device.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

namespace ble_server {
namespace {

constexpr const char* TAG = "ble";
constexpr int kMaxSessions = 3;
constexpr uint16_t kAppId = 0x55;
enum { IDX_SVC, IDX_RX_DECL, IDX_RX_VAL, IDX_TX_DECL, IDX_TX_VAL, IDX_TX_CCCD, IDX_INFO_DECL, IDX_INFO_VAL, IDX_NB };

const uint16_t kPrimaryUuid = ESP_GATT_UUID_PRI_SERVICE;
const uint16_t kCharDeclUuid = ESP_GATT_UUID_CHAR_DECLARE;
const uint16_t kCccdUuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
const uint8_t kPropWrite = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
const uint8_t kPropNotify = ESP_GATT_CHAR_PROP_BIT_NOTIFY | ESP_GATT_CHAR_PROP_BIT_READ;
const uint8_t kPropRead = ESP_GATT_CHAR_PROP_BIT_READ;
uint8_t cccd_val[2] = {0, 0};
uint8_t rx_buf[1] = {0};
uint8_t tx_buf[1] = {0};
uint8_t info_buf[1] = {0};

const esp_gatts_attr_db_t kDb[IDX_NB] = {
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&kPrimaryUuid, ESP_GATT_PERM_READ, 16, 16, (uint8_t*)swp::kServiceUuid128}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&kCharDeclUuid, ESP_GATT_PERM_READ, 1, 1, (uint8_t*)&kPropWrite}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t*)swp::kRxCharUuid128, ESP_GATT_PERM_WRITE_ENC_MITM, 512, 1, rx_buf}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&kCharDeclUuid, ESP_GATT_PERM_READ, 1, 1, (uint8_t*)&kPropNotify}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_128, (uint8_t*)swp::kTxCharUuid128, ESP_GATT_PERM_READ_ENC_MITM, 512, 1, tx_buf}},
    {{ESP_GATT_AUTO_RSP},
     {ESP_UUID_LEN_16, (uint8_t*)&kCccdUuid, ESP_GATT_PERM_READ_ENC_MITM | ESP_GATT_PERM_WRITE_ENC_MITM, 2, 2, cccd_val}},
    {{ESP_GATT_AUTO_RSP}, {ESP_UUID_LEN_16, (uint8_t*)&kCharDeclUuid, ESP_GATT_PERM_READ, 1, 1, (uint8_t*)&kPropRead}},
    // Open (unauthenticated) identity characteristic, answered by the app.
    {{ESP_GATT_RSP_BY_APP}, {ESP_UUID_LEN_128, (uint8_t*)swp::kInfoCharUuid128, ESP_GATT_PERM_READ, 128, 1, info_buf}},
};

uint16_t handles[IDX_NB] = {};
esp_gatt_if_t gif = ESP_GATT_IF_NONE;
std::mutex mu;
Session sessions[kMaxSessions];
std::atomic<int64_t> pairing_until_us{0};
std::atomic<bool> congested{false};
bool advertising = false;

struct TxItem {
    uint16_t conn_id;
    std::vector<uint8_t>* frame;
};
QueueHandle_t txq;
std::atomic<int> queued_per_conn[kMaxSessions];
std::atomic<uint16_t> seq{1};

// Decoders live with the sessions but are only touched from the BTC task.
swp::FrameDecoder decoders[kMaxSessions];

int slot_of(uint16_t conn_id) {
    for (int i = 0; i < kMaxSessions; ++i)
        if (sessions[i].conn_id == conn_id) return i;
    return -1;
}

uint8_t adv_raw[31];
uint8_t adv_len = 0;
uint8_t rsp_raw[31];
uint8_t rsp_len = 0;

void build_adv() {
    adv_len = 0;
    adv_raw[adv_len++] = 2;
    adv_raw[adv_len++] = ESP_BLE_AD_TYPE_FLAG;
    adv_raw[adv_len++] = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
    adv_raw[adv_len++] = 17;
    adv_raw[adv_len++] = ESP_BLE_AD_TYPE_128SRV_CMPL;
    memcpy(adv_raw + adv_len, swp::kServiceUuid128, 16);
    adv_len += 16;
    std::string name = config::get(config::kDeviceName, "SW Companion");
    if (name.size() > 29) name.resize(29);
    rsp_len = 0;
    rsp_raw[rsp_len++] = static_cast<uint8_t>(name.size() + 1);
    rsp_raw[rsp_len++] = ESP_BLE_AD_TYPE_NAME_CMPL;
    memcpy(rsp_raw + rsp_len, name.data(), name.size());
    rsp_len += static_cast<uint8_t>(name.size());
}

void start_adv() {
    int n = 0;
    for (auto& s : sessions) n += s.conn_id != 0xFFFF;
    if (n >= 2) return;  // watch + one config client
    esp_ble_adv_params_t p = {};
    // 500-1000 ms: discoverable within a few seconds without constant radio use.
    p.adv_int_min = 0x320;
    p.adv_int_max = 0x640;
    p.adv_type = ADV_TYPE_IND;
    p.own_addr_type = BLE_ADDR_TYPE_PUBLIC;
    p.channel_map = ADV_CHNL_ALL;
    p.adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY;
    esp_err_t err = esp_ble_gap_start_advertising(&p);
    if (err == ESP_OK) advertising = true;
    else ESP_LOGW(TAG, "start advertising failed: %s", esp_err_to_name(err));
}

bool is_bonded(const uint8_t* bda) {
    int n = esp_ble_get_bond_device_num();
    if (n <= 0) return false;
    std::vector<esp_ble_bond_dev_t> list(n);
    esp_ble_get_bond_device_list(&n, list.data());
    for (int i = 0; i < n; ++i)
        if (memcmp(list[i].bd_addr, bda, 6) == 0) return true;
    return false;
}

void gap_cb(esp_gap_ble_cb_event_t ev, esp_ble_gap_cb_param_t* p) {
    switch (ev) {
        case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
        case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
            if (p->adv_data_raw_cmpl.status != ESP_BT_STATUS_SUCCESS) ESP_LOGE(TAG, "advertising data rejected: %d", p->adv_data_raw_cmpl.status);
            start_adv();
            break;
        case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
            if (p->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
                const uint8_t* a = esp_bt_dev_get_address();
                ESP_LOGI(TAG, "advertising as \"%s\" (%02x:%02x:%02x:%02x:%02x:%02x)", config::get(config::kDeviceName, "SW Companion").c_str(),
                         a[0], a[1], a[2], a[3], a[4], a[5]);
            } else {
                advertising = false;
                ESP_LOGE(TAG, "advertising start failed: status %d", p->adv_start_cmpl.status);
            }
            break;
        case ESP_GAP_BLE_SEC_REQ_EVT: {
            bool accept = pairing_window_open() || is_bonded(p->ble_security.ble_req.bd_addr);
            if (!accept) ESP_LOGW(TAG, "pairing rejected: window closed (press BOOT to open it)");
            esp_ble_gap_security_rsp(p->ble_security.ble_req.bd_addr, accept);
            break;
        }
        case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
            ESP_LOGI(TAG, "pairing: enter passkey %06lu on the other device", (unsigned long)p->ble_security.key_notif.passkey);
            break;
        case ESP_GAP_BLE_AUTH_CMPL_EVT: {
            auto& a = p->ble_security.auth_cmpl;
            bool mitm = (a.auth_mode & ESP_LE_AUTH_REQ_MITM) != 0;
            std::lock_guard<std::mutex> g(mu);
            for (auto& s : sessions)
                if (memcmp(s.bda, a.bd_addr, 6) == 0) s.authenticated = a.success && mitm;
            ESP_LOGI(TAG, "authentication %s (mode 0x%x)", a.success && mitm ? "ok" : "FAILED", a.auth_mode);
            break;
        }
        default:
            break;
    }
}

void send_info_rsp(esp_ble_gatts_cb_param_t* p) {
    swp::DeviceInfo d;
    d.role = static_cast<uint8_t>(swp::Role::Companion);
    d.name = config::get(config::kDeviceName, "SW Companion");
    d.firmware = esp_app_get_description()->version;
    d.hardware = "Freenove FNK0060 ESP32-WROVER";
    esp_read_mac(d.mac, ESP_MAC_BT);
    auto payload = swp::encode_payload(d);
    esp_gatt_rsp_t rsp = {};
    rsp.attr_value.handle = p->read.handle;
    size_t off = p->read.offset;
    size_t n = off < payload.size() ? std::min<size_t>(payload.size() - off, ESP_GATT_MAX_ATTR_LEN) : 0;
    rsp.attr_value.len = static_cast<uint16_t>(n);
    if (n) memcpy(rsp.attr_value.value, payload.data() + off, n);
    esp_ble_gatts_send_response(gif, p->read.conn_id, p->read.trans_id, ESP_GATT_OK, &rsp);
}

void gatts_cb(esp_gatts_cb_event_t ev, esp_gatt_if_t ifc, esp_ble_gatts_cb_param_t* p) {
    switch (ev) {
        case ESP_GATTS_REG_EVT:
            gif = ifc;
            build_adv();
            esp_ble_gap_set_device_name(config::get(config::kDeviceName, "SW Companion").c_str());
            esp_ble_gap_config_adv_data_raw(adv_raw, adv_len);
            esp_ble_gap_config_scan_rsp_data_raw(rsp_raw, rsp_len);
            esp_ble_gatts_create_attr_tab(kDb, ifc, IDX_NB, 0);
            break;
        case ESP_GATTS_CREAT_ATTR_TAB_EVT:
            if (p->add_attr_tab.status == ESP_GATT_OK && p->add_attr_tab.num_handle == IDX_NB) {
                memcpy(handles, p->add_attr_tab.handles, sizeof handles);
                esp_ble_gatts_start_service(handles[IDX_SVC]);
            } else {
                ESP_LOGE(TAG, "attribute table creation failed: 0x%x", p->add_attr_tab.status);
            }
            break;
        case ESP_GATTS_CONNECT_EVT: {
            std::lock_guard<std::mutex> g(mu);
            for (int i = 0; i < kMaxSessions; ++i)
                if (sessions[i].conn_id == 0xFFFF) {
                    sessions[i] = Session{};
                    sessions[i].conn_id = p->connect.conn_id;
                    memcpy(sessions[i].bda, p->connect.remote_bda, 6);
                    decoders[i].reset();
                    queued_per_conn[i].store(0);
                    break;
                }
            advertising = false;
            ESP_LOGI(TAG, "connected conn %u", p->connect.conn_id);
            // Ask for encryption right away; bonded peers re-encrypt silently.
            esp_ble_set_encryption(p->connect.remote_bda, ESP_BLE_SEC_ENCRYPT_MITM);
            esp_ble_conn_update_params_t cp = {};
            memcpy(cp.bda, p->connect.remote_bda, 6);
            cp.min_int = 24;
            cp.max_int = 40;
            cp.latency = 4;
            cp.timeout = 400;
            esp_ble_gap_update_conn_params(&cp);
            break;
        }
        case ESP_GATTS_DISCONNECT_EVT: {
            uint8_t role = 0;
            {
                std::lock_guard<std::mutex> g(mu);
                int i = slot_of(p->disconnect.conn_id);
                if (i >= 0) {
                    role = sessions[i].role;
                    sessions[i] = Session{};
                    decoders[i].reset();
                }
            }
            ESP_LOGI(TAG, "disconnected conn %u (reason 0x%x)", p->disconnect.conn_id, p->disconnect.reason);
            congested.store(false);
            dispatch::on_disconnect(p->disconnect.conn_id, role);
            start_adv();
            break;
        }
        case ESP_GATTS_MTU_EVT: {
            std::lock_guard<std::mutex> g(mu);
            int i = slot_of(p->mtu.conn_id);
            if (i >= 0) sessions[i].mtu = p->mtu.mtu;
            break;
        }
        case ESP_GATTS_READ_EVT:
            if (p->read.handle == handles[IDX_INFO_VAL]) send_info_rsp(p);
            break;
        case ESP_GATTS_WRITE_EVT: {
            int i;
            bool authed;
            {
                std::lock_guard<std::mutex> g(mu);
                i = slot_of(p->write.conn_id);
                authed = i >= 0 && sessions[i].authenticated;
                if (i >= 0 && p->write.handle == handles[IDX_TX_CCCD] && p->write.len == 2) sessions[i].notify = p->write.value[0] & 1;
            }
            if (i < 0 || p->write.handle != handles[IDX_RX_VAL]) break;
            if (!authed) break;  // permissions already require MITM; defence in depth
            decoders[i].feed(p->write.value, p->write.len);
            swp::Frame f;
            swp::FrameDecoder::Result r;
            while ((r = decoders[i].next(f)) != swp::FrameDecoder::Result::NeedMore)
                if (r == swp::FrameDecoder::Result::Frame) dispatch::on_frame(p->write.conn_id, f);
            break;
        }
        case ESP_GATTS_CONGEST_EVT:
            congested.store(p->congest.congested);
            break;
        default:
            break;
    }
}

void tx_task(void*) {
    TxItem it;
    for (;;) {
        if (xQueueReceive(txq, &it, portMAX_DELAY) != pdTRUE) continue;
        uint16_t mtu_v;
        bool ok;
        int slot;
        {
            std::lock_guard<std::mutex> g(mu);
            slot = slot_of(it.conn_id);
            ok = slot >= 0 && sessions[slot].authenticated && sessions[slot].notify;
            mtu_v = slot >= 0 ? sessions[slot].mtu : 23;
        }
        if (ok) {
            size_t chunk = mtu_v > 3 ? mtu_v - 3 : 20;
            auto& f = *it.frame;
            for (size_t off = 0; off < f.size();) {
                // Back off while the link is congested; bounded so a dead link cannot stall.
                for (int w = 0; congested.load() && w < 200; ++w) vTaskDelay(pdMS_TO_TICKS(5));
                size_t n = std::min(chunk, f.size() - off);
                if (esp_ble_gatts_send_indicate(gif, it.conn_id, handles[IDX_TX_VAL], n, f.data() + off, false) != ESP_OK) {
                    vTaskDelay(pdMS_TO_TICKS(10));
                    if (slot_of(it.conn_id) < 0) break;
                    continue;
                }
                off += n;
            }
        }
        if (slot >= 0) queued_per_conn[slot].fetch_sub(1);
        delete it.frame;
    }
}

void IRAM_ATTR boot_isr(void*) { pairing_until_us.store(esp_timer_get_time() + 120LL * 1000000); }

}  // namespace

void init() {
    for (auto& s : sessions) s = Session{};
    for (auto& q : queued_per_conn) q.store(0);
    txq = xQueueCreate(32, sizeof(TxItem));
    xTaskCreatePinnedToCore(tx_task, "ble_tx", 3072, nullptr, 9, nullptr, 0);

    esp_ble_gap_register_callback(gap_cb);
    esp_ble_gatts_register_callback(gatts_cb);
    esp_ble_gatt_set_local_mtu(247);

    esp_ble_auth_req_t auth = ESP_LE_AUTH_REQ_SC_MITM_BOND;
    esp_ble_io_cap_t iocap = ESP_IO_CAP_OUT;  // display-only: static passkey
    uint8_t key_size = 16;
    uint8_t init_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint8_t rsp_key = ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK;
    uint32_t passkey = config::ble_passkey();
    uint8_t only_specified = ESP_BLE_ONLY_ACCEPT_SPECIFIED_AUTH_ENABLE;
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_STATIC_PASSKEY, &passkey, sizeof passkey);
    esp_ble_gap_set_security_param(ESP_BLE_SM_AUTHEN_REQ_MODE, &auth, sizeof auth);
    esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap, sizeof iocap);
    esp_ble_gap_set_security_param(ESP_BLE_SM_MAX_KEY_SIZE, &key_size, sizeof key_size);
    esp_ble_gap_set_security_param(ESP_BLE_SM_ONLY_ACCEPT_SPECIFIED_SEC_AUTH, &only_specified, sizeof only_specified);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_INIT_KEY, &init_key, sizeof init_key);
    esp_ble_gap_set_security_param(ESP_BLE_SM_SET_RSP_KEY, &rsp_key, sizeof rsp_key);
    ESP_LOGI(TAG, "BLE pairing passkey: %06lu (pairing window %d s after boot; press BOOT to reopen)", (unsigned long)passkey,
             CONFIG_COMPANION_PAIRING_WINDOW_S);
    open_pairing_window(CONFIG_COMPANION_PAIRING_WINDOW_S);

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << GPIO_NUM_0;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    io.intr_type = GPIO_INTR_NEGEDGE;
    gpio_config(&io);
    gpio_install_isr_service(0);
    gpio_isr_handler_add(GPIO_NUM_0, boot_isr, nullptr);

    esp_ble_gatts_app_register(kAppId);
}

bool send(uint16_t conn_id, uint8_t type, std::vector<uint8_t>&& payload) {
    int slot;
    {
        std::lock_guard<std::mutex> g(mu);
        slot = slot_of(conn_id);
        if (slot < 0 || !sessions[slot].authenticated) return false;
    }
    auto* f = new (std::nothrow) std::vector<uint8_t>();
    if (!f || !swp::encode_frame(type, seq.fetch_add(1), payload, *f)) {
        delete f;
        return false;
    }
    queued_per_conn[slot].fetch_add(1);
    TxItem it{conn_id, f};
    if (xQueueSend(txq, &it, pdMS_TO_TICKS(50)) != pdTRUE) {
        queued_per_conn[slot].fetch_sub(1);
        delete f;
        ESP_LOGW(TAG, "tx queue full; dropped 0x%02x", type);
        return false;
    }
    return true;
}

bool to_watch(uint8_t type, std::vector<uint8_t>&& payload) {
    uint16_t c = watch_conn();
    return c != 0xFFFF && send(c, type, std::move(payload));
}

uint16_t watch_conn() {
    std::lock_guard<std::mutex> g(mu);
    for (auto& s : sessions)
        if (s.conn_id != 0xFFFF && s.authenticated && s.role == static_cast<uint8_t>(swp::Role::Watch)) return s.conn_id;
    return 0xFFFF;
}

bool watch_connected() { return watch_conn() != 0xFFFF; }

uint16_t mtu(uint16_t conn_id) {
    std::lock_guard<std::mutex> g(mu);
    int i = slot_of(conn_id);
    return i >= 0 ? sessions[i].mtu : 23;
}

int queued(uint16_t conn_id) {
    std::lock_guard<std::mutex> g(mu);
    int i = slot_of(conn_id);
    return i >= 0 ? queued_per_conn[i].load() : 0;
}

void set_role(uint16_t conn_id, uint8_t r) {
    std::lock_guard<std::mutex> g(mu);
    int i = slot_of(conn_id);
    if (i >= 0) sessions[i].role = r;
}

uint8_t role(uint16_t conn_id) {
    std::lock_guard<std::mutex> g(mu);
    int i = slot_of(conn_id);
    return i >= 0 ? sessions[i].role : 0;
}

void open_pairing_window(uint32_t seconds) { pairing_until_us.store(esp_timer_get_time() + static_cast<int64_t>(seconds) * 1000000); }
bool pairing_window_open() { return esp_timer_get_time() < pairing_until_us.load(); }

}  // namespace ble_server
