#include "ble/peripheral.hpp"

#include <atomic>
#include <cstring>

#include "app/state.hpp"
#include "ble/link.hpp"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "swp/messages.hpp"

namespace peripheral {
namespace {

constexpr const char* TAG = "periph";

std::atomic<uint16_t> conn{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> authed{false};
std::atomic<uint16_t> cur_mtu{23};
uint16_t rx_val = 0, tx_val = 0, info_val = 0;
bool synced = false;

ble_uuid128_t make_uuid(const uint8_t (&b)[16]) {
    ble_uuid128_t u;
    u.u.type = BLE_UUID_TYPE_128;
    memcpy(u.value, b, 16);
    return u;
}
ble_uuid128_t svc_uuid = make_uuid(swp::kServiceUuid128);
ble_uuid128_t rx_uuid = make_uuid(swp::kRxCharUuid128);
ble_uuid128_t tx_uuid = make_uuid(swp::kTxCharUuid128);
ble_uuid128_t info_uuid = make_uuid(swp::kInfoCharUuid128);

int access_cb(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt* ctxt, void*) {
    if (attr_handle == info_val && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        // Unauthenticated identity only: lets the web app show what it found before pairing.
        swp::DeviceInfo d;
        d.role = static_cast<uint8_t>(swp::Role::Watch);
        d.name = "SW Watch";
        d.firmware = esp_app_get_description()->version;
        d.hardware = "Waveshare ESP32-S3-Touch-AMOLED-2.06";
        esp_read_mac(d.mac, ESP_MAC_BT);
        auto p = swp::encode_payload(d);
        return os_mbuf_append(ctxt->om, p.data(), p.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (attr_handle == rx_val && ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        // The stack already enforces encryption+MITM via the characteristic flags; this is
        // a defence-in-depth check.
        if (!authed.load() || conn_handle != conn.load()) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        uint8_t buf[512];
        uint16_t n = 0;
        if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof buf, &n) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        swlink::on_web_bytes(buf, n);
        return 0;
    }
    if (attr_handle == tx_val && ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) return 0;
    return BLE_ATT_ERR_UNLIKELY;
}

const struct ble_gatt_chr_def chrs[] = {
    {.uuid = &rx_uuid.u,
     .access_cb = access_cb,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN,
     .val_handle = &rx_val},
    {.uuid = &tx_uuid.u,
     .access_cb = access_cb,
     .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC | BLE_GATT_CHR_F_READ_AUTHEN,
     .val_handle = &tx_val},
    {.uuid = &info_uuid.u, .access_cb = access_cb, .flags = BLE_GATT_CHR_F_READ, .val_handle = &info_val},
    {},
};

const struct ble_gatt_svc_def svcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &svc_uuid.u, .characteristics = chrs},
    {},
};

void set_web_state(bool adv, bool connected) {
    {
        app::Locked s;
        s->web_advertising = adv;
        s->web_connected = connected;
    }
    app::mark(app::kDirtyWebConfig);
}

int gap_event(struct ble_gap_event* ev, void*) {
    switch (ev->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (ev->connect.status == 0) {
                conn.store(ev->connect.conn_handle);
                authed.store(false);
                set_web_state(false, true);
                ESP_LOGI(TAG, "web config client connected");
            } else {
                set_web_state(false, false);
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            conn.store(BLE_HS_CONN_HANDLE_NONE);
            authed.store(false);
            cur_mtu.store(23);
            swlink::on_web_down();
            {
                app::Locked s;
                if (s->passkey_mode == app::PasskeyMode::DisplayForWeb || s->passkey_mode == app::PasskeyMode::ConfirmForWeb)
                    s->passkey_mode = app::PasskeyMode::None;
            }
            app::mark(app::kDirtyPasskey);
            set_web_state(false, false);
            return 0;
        case BLE_GAP_EVENT_ADV_COMPLETE:
            set_web_state(false, conn.load() != BLE_HS_CONN_HANDLE_NONE);
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE: {
            struct ble_gap_conn_desc d;
            if (ev->enc_change.status == 0 && ble_gap_conn_find(ev->enc_change.conn_handle, &d) == 0 && d.sec_state.encrypted &&
                d.sec_state.authenticated) {
                authed.store(true);
                swlink::on_web_connected();
                ESP_LOGI(TAG, "web config link authenticated");
            }
            {
                app::Locked s;
                s->passkey_mode = app::PasskeyMode::None;
            }
            app::mark(app::kDirtyPasskey);
            return 0;
        }
        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            struct ble_sm_io io = {};
            io.action = ev->passkey.params.action;
            if (io.action == BLE_SM_IOACT_DISP) {
                io.passkey = esp_random() % 1000000;
                {
                    app::Locked s;
                    s->passkey_mode = app::PasskeyMode::DisplayForWeb;
                    s->passkey_value = io.passkey;
                }
                app::mark(app::kDirtyPasskey);
                ble_sm_inject_io(ev->passkey.conn_handle, &io);
            } else if (io.action == BLE_SM_IOACT_NUMCMP) {
                app::Locked s;
                s->passkey_mode = app::PasskeyMode::ConfirmForWeb;
                s->passkey_value = ev->passkey.params.numcmp;
                app::mark(app::kDirtyPasskey);
            } else {
                io.numcmp_accept = 0;
                ble_sm_inject_io(ev->passkey.conn_handle, &io);
            }
            return 0;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc d;
            if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &d) == 0) ble_store_util_delete_peer(&d.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_MTU:
            cur_mtu.store(ev->mtu.value);
            return 0;
        default:
            return 0;
    }
}

}  // namespace

void register_services() {
    int rc = ble_gatts_count_cfg(svcs);
    if (rc == 0) rc = ble_gatts_add_svcs(svcs);
    if (rc != 0) ESP_LOGE(TAG, "GATT registration failed: %d", rc);
}

void on_sync() { synced = true; }

void start_advertising(uint32_t seconds) {
    if (!synced || conn.load() != BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_hs_adv_fields f = {};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128 = &svc_uuid;
    f.num_uuids128 = 1;
    f.uuids128_is_complete = 1;
    if (ble_gap_adv_set_fields(&f) != 0) return;
    struct ble_hs_adv_fields rsp = {};
    const char* name = "SW Watch";
    rsp.name = reinterpret_cast<const uint8_t*>(name);
    rsp.name_len = strlen(name);
    rsp.name_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    struct ble_gap_adv_params p = {};
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    p.itvl_min = 160;  // 100 ms: quick discovery during the short window
    p.itvl_max = 240;
    uint8_t own;
    ble_hs_id_infer_auto(0, &own);
    if (ble_gap_adv_start(own, nullptr, static_cast<int32_t>(seconds * 1000), &p, gap_event, nullptr) == 0) set_web_state(true, false);
}

void stop_advertising() {
    ble_gap_adv_stop();
    set_web_state(false, conn.load() != BLE_HS_CONN_HANDLE_NONE);
}

bool connected() { return conn.load() != BLE_HS_CONN_HANDLE_NONE && authed.load(); }
uint16_t mtu() { return cur_mtu.load(); }

bool notify(const uint8_t* data, size_t len) {
    for (int tries = 0; tries < 200; ++tries) {
        uint16_t h = conn.load();
        if (h == BLE_HS_CONN_HANDLE_NONE || !authed.load()) return false;
        struct os_mbuf* om = ble_hs_mbuf_from_flat(data, static_cast<uint16_t>(len));
        if (om) {
            int rc = ble_gatts_notify_custom(h, tx_val, om);
            if (rc == 0) return true;
            if (rc != BLE_HS_ENOMEM) return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return false;
}

void confirm_numeric(bool accept) {
    uint16_t h = conn.load();
    if (h == BLE_HS_CONN_HANDLE_NONE) return;
    struct ble_sm_io io = {};
    io.action = BLE_SM_IOACT_NUMCMP;
    io.numcmp_accept = accept ? 1 : 0;
    ble_sm_inject_io(h, &io);
    app::Locked s;
    s->passkey_mode = app::PasskeyMode::None;
    app::mark(app::kDirtyPasskey);
}

}  // namespace peripheral
