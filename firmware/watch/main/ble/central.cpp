#include "ble/central.hpp"

#include <atomic>
#include <cstring>

#include "app/settings.hpp"
#include "app/state.hpp"
#include "ble/link.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "swp/messages.hpp"

extern "C" void ble_store_config_init(void);

namespace central {
namespace {

constexpr const char* TAG = "central";
// Reconnection schedule (seconds) after each failed attempt; each attempt itself scans for
// at most kConnectTimeoutMs, so the radio duty cycle stays low when the companion is away.
constexpr uint32_t kBackoffS[] = {2, 5, 10, 20, 40, 60, 120, 300};
constexpr int32_t kConnectTimeoutMs = 8000;
constexpr int32_t kScanMs = 12000;

enum class Cmd : uint8_t { Scan, StopScan, Pair, Disconnect, Reconnect, Forget, Passkey, CancelPasskey, Drop, Evaluate };
struct Command {
    Cmd cmd;
    uint8_t addr[6];
    uint8_t addr_type;
    uint32_t value;
    char name[32];
};

QueueHandle_t cmdq = nullptr;
uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
std::atomic<bool> synced{false};
std::atomic<uint16_t> conn{BLE_HS_CONN_HANDLE_NONE};
std::atomic<uint16_t> cur_mtu{23};
std::atomic<bool> fast_wanted{false};

// Idle: 30-50 ms with peripheral latency (low power). Fast (Maps open): 15-20 ms, no latency,
// for tile transfers.
void apply_conn_params(uint16_t h) {
    bool fast = fast_wanted.load();
    struct ble_gap_upd_params p = {};
    p.itvl_min = fast ? 12 : 24;
    p.itvl_max = fast ? 16 : 40;
    p.latency = fast ? 0 : 4;
    p.supervision_timeout = 400;
    int rc = ble_gap_update_params(h, &p);
    if (rc != 0) ESP_LOGD(TAG, "conn param update: %d", rc);
}
uint16_t rx_handle = 0, tx_handle = 0, tx_cccd = 0, svc_start = 0, svc_end = 0;

// Manager-owned state (only touched by mgr_task, except where noted).
settings::Peer peer;
bool user_disconnected = false;
bool pairing_new = false;  // connecting to a newly selected peer: save it on success
uint32_t attempt = 0;
TickType_t next_attempt = 0;
bool attempt_pending = false;

ble_uuid128_t make_uuid(const uint8_t (&b)[16]) {
    ble_uuid128_t u;
    u.u.type = BLE_UUID_TYPE_128;
    memcpy(u.value, b, 16);
    return u;
}
const ble_uuid128_t kSvcUuid = make_uuid(swp::kServiceUuid128);
const ble_uuid128_t kRxUuid = make_uuid(swp::kRxCharUuid128);
const ble_uuid128_t kTxUuid = make_uuid(swp::kTxCharUuid128);

void post(Cmd c, uint32_t value = 0) {
    Command m = {};
    m.cmd = c;
    m.value = value;
    xQueueSend(cmdq, &m, 0);
}

void set_state(app::LinkState st, const char* err = nullptr) {
    {
        app::Locked s;
        s->link.state = st;
        if (err) s->link.last_error = err;
    }
    app::mark(app::kDirtyLink);
}

int gap_event(struct ble_gap_event* ev, void* arg);

ble_addr_t peer_addr() {
    ble_addr_t a;
    a.type = peer.addr_type;
    memcpy(a.val, peer.addr, 6);
    return a;
}

void schedule_retry() {
    if (!peer.valid || user_disconnected) return;
    uint32_t idx = attempt < sizeof(kBackoffS) / sizeof(kBackoffS[0]) ? attempt : sizeof(kBackoffS) / sizeof(kBackoffS[0]) - 1;
    uint32_t delay_s = kBackoffS[idx];
    // +/-20% jitter so a watch and companion rebooting together do not stay in lockstep.
    delay_s = delay_s * (80 + esp_random() % 41) / 100;
    if (delay_s == 0) delay_s = 1;
    ++attempt;
    next_attempt = xTaskGetTickCount() + pdMS_TO_TICKS(delay_s * 1000);
    attempt_pending = true;
    {
        app::Locked s;
        s->link.state = app::LinkState::Waiting;
        s->link.retry_in_s = delay_s;
        s->link.attempts = attempt;
    }
    app::mark(app::kDirtyLink);
}

void try_connect() {
    attempt_pending = false;
    if (!synced.load() || !peer.valid || user_disconnected || conn.load() != BLE_HS_CONN_HANDLE_NONE) return;
    if (ble_gap_disc_active()) ble_gap_disc_cancel();
    ble_addr_t a = peer_addr();
    struct ble_gap_conn_params p = {};
    p.scan_itvl = 0x0060;  // 60 ms
    p.scan_window = 0x0030;
    p.itvl_min = 24;  // 30 ms
    p.itvl_max = 40;  // 50 ms
    p.latency = 4;    // peripheral may skip events when idle
    p.supervision_timeout = 400;  // 4 s
    p.min_ce_len = 0;
    p.max_ce_len = 0;
    int rc = ble_gap_connect(own_addr_type, &a, kConnectTimeoutMs, &p, gap_event, nullptr);
    if (rc != 0) {
        ESP_LOGW(TAG, "connect start failed: %d", rc);
        schedule_retry();
        return;
    }
    set_state(app::LinkState::Connecting);
}

// ---- GATT discovery chain ------------------------------------------------------------

int on_cccd_written(uint16_t conn_handle, const struct ble_gatt_error* err, struct ble_gatt_attr*, void*) {
    if (err->status != 0) {
        ESP_LOGE(TAG, "subscribe failed: %d", err->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        return 0;
    }
    if (pairing_new) {
        settings::set_peer(peer);
        pairing_new = false;
        app::Locked s;
        s->link.has_peer = true;
    }
    attempt = 0;
    swlink::on_companion_ready();
    return 0;
}

int on_dsc(uint16_t conn_handle, const struct ble_gatt_error* err, uint16_t, const struct ble_gatt_dsc* dsc, void*) {
    if (err->status == 0) {
        static const ble_uuid16_t cccd_uuid = {{BLE_UUID_TYPE_16}, BLE_GATT_DSC_CLT_CFG_UUID16};
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd_uuid.u) == 0) tx_cccd = dsc->handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && tx_cccd) {
        uint8_t v[2] = {1, 0};
        ble_gattc_write_flat(conn_handle, tx_cccd, v, 2, on_cccd_written, nullptr);
    } else {
        ESP_LOGE(TAG, "CCCD not found (%d)", err->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

int on_chr(uint16_t conn_handle, const struct ble_gatt_error* err, const struct ble_gatt_chr* chr, void*) {
    if (err->status == 0) {
        if (ble_uuid_cmp(&chr->uuid.u, &kRxUuid.u) == 0) rx_handle = chr->val_handle;
        if (ble_uuid_cmp(&chr->uuid.u, &kTxUuid.u) == 0) tx_handle = chr->val_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && rx_handle && tx_handle) {
        ble_gattc_disc_all_dscs(conn_handle, tx_handle, svc_end, on_dsc, nullptr);
    } else {
        ESP_LOGE(TAG, "SWP characteristics missing (%d)", err->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

int on_svc(uint16_t conn_handle, const struct ble_gatt_error* err, const struct ble_gatt_svc* svc, void*) {
    if (err->status == 0) {
        svc_start = svc->start_handle;
        svc_end = svc->end_handle;
        return 0;
    }
    if (err->status == BLE_HS_EDONE && svc_start) {
        ble_gattc_disc_all_chrs(conn_handle, svc_start, svc_end, on_chr, nullptr);
    } else {
        ESP_LOGE(TAG, "SWP service not found (%d)", err->status);
        ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

void start_discovery(uint16_t c) {
    rx_handle = tx_handle = tx_cccd = svc_start = svc_end = 0;
    set_state(app::LinkState::Discovering);
    ble_gattc_disc_svc_by_uuid(c, &kSvcUuid.u, on_svc, nullptr);
}

int on_mtu(uint16_t, const struct ble_gatt_error* err, uint16_t mtu, void*) {
    if (err->status == 0) {
        cur_mtu.store(mtu);
        app::Locked s;
        s->link.mtu = mtu;
    }
    return 0;
}

// ---- GAP -----------------------------------------------------------------------------

bool adv_has_service(const struct ble_hs_adv_fields& f) {
    for (int i = 0; i < f.num_uuids128; ++i)
        if (ble_uuid_cmp(&f.uuids128[i].u, &kSvcUuid.u) == 0) return true;
    return false;
}

// Scan diagnostics: total reports seen vs. those carrying the companion service UUID.
uint32_t scan_reports = 0, scan_matches = 0;

void on_disc(const struct ble_gap_disc_desc& d) {
    ++scan_reports;
    struct ble_hs_adv_fields f;
    if (ble_hs_adv_parse_fields(&f, d.data, d.length_data) != 0) return;
    std::string name = f.name ? std::string(reinterpret_cast<const char*>(f.name), f.name_len) : "";
    app::Locked s;
    for (auto& e : s->scan)
        if (memcmp(e.addr, d.addr.val, 6) == 0) {
            e.rssi = d.rssi;
            if (!name.empty()) e.name = name;  // names usually arrive in the scan response
            app::mark(app::kDirtyScan);
            return;
        }
    if (!adv_has_service(f) || s->scan.size() >= 10) return;
    ++scan_matches;
    ESP_LOGI(TAG, "found companion %02x:%02x:%02x:%02x:%02x:%02x rssi %d", d.addr.val[5], d.addr.val[4], d.addr.val[3], d.addr.val[2],
             d.addr.val[1], d.addr.val[0], d.rssi);
    app::ScanEntry e;
    memcpy(e.addr, d.addr.val, 6);
    e.addr_type = d.addr.type;
    e.name = name.empty() ? "Companion" : name;
    e.rssi = d.rssi;
    s->scan.push_back(e);
    app::mark(app::kDirtyScan);
}

int gap_event(struct ble_gap_event* ev, void*) {
    switch (ev->type) {
        case BLE_GAP_EVENT_DISC:
            on_disc(ev->disc);
            return 0;
        case BLE_GAP_EVENT_DISC_COMPLETE: {
            ESP_LOGI(TAG, "scan done (reason %d): %lu advert reports, %lu companion(s)", ev->disc_complete.reason,
                     static_cast<unsigned long>(scan_reports), static_cast<unsigned long>(scan_matches));
            app::Locked s;
            s->scanning = false;
            app::mark(app::kDirtyScan);
            return 0;
        }
        case BLE_GAP_EVENT_CONNECT:
            if (ev->connect.status == 0) {
                conn.store(ev->connect.conn_handle);
                ESP_LOGI(TAG, "connected; securing link");
                set_state(app::LinkState::Securing);
                ble_gattc_exchange_mtu(ev->connect.conn_handle, on_mtu, nullptr);
                // 251-byte link-layer packets: one ATT notification per packet instead of ~10.
                ble_gap_set_data_len(ev->connect.conn_handle, 251, 2120);
                if (fast_wanted.load()) apply_conn_params(ev->connect.conn_handle);
                // Encrypts with the stored LTK when bonded, otherwise starts passkey pairing.
                ble_gap_security_initiate(ev->connect.conn_handle);
            } else {
                ESP_LOGI(TAG, "connect attempt failed: %d", ev->connect.status);
                post(Cmd::Evaluate, 1);
            }
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected: reason 0x%x", ev->disconnect.reason);
            conn.store(BLE_HS_CONN_HANDLE_NONE);
            cur_mtu.store(23);
            swlink::on_companion_down();
            {
                app::Locked s;
                s->passkey_mode = app::PasskeyMode::None;
            }
            app::mark(app::kDirtyPasskey);
            post(Cmd::Evaluate, 1);
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            if (ev->enc_change.status == 0) {
                struct ble_gap_conn_desc desc;
                if (ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0 && !desc.sec_state.authenticated) {
                    // Refuse unauthenticated (Just Works) links: the companion holds secrets.
                    ESP_LOGW(TAG, "link not MITM-authenticated; dropping");
                    set_state(app::LinkState::Securing, "Pairing was not authenticated");
                    ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_AUTH_FAIL);
                    return 0;
                }
                start_discovery(ev->enc_change.conn_handle);
            } else {
                ESP_LOGW(TAG, "encryption failed: %d", ev->enc_change.status);
                if (ev->enc_change.status == BLE_HS_HCI_ERR(BLE_ERR_PINKEY_MISSING)) {
                    // The companion lost our bond (reflashed/forgot). Start a fresh pairing; the
                    // user must enter the companion passkey again, so this cannot be hijacked.
                    struct ble_gap_conn_desc desc;
                    if (ble_gap_conn_find(ev->enc_change.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
                    ble_gap_security_initiate(ev->enc_change.conn_handle);
                    return 0;
                }
                set_state(app::LinkState::Securing, "Pairing failed");
                ble_gap_terminate(ev->enc_change.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            }
            return 0;
        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            app::Locked s;
            if (ev->passkey.params.action == BLE_SM_IOACT_INPUT) {
                s->passkey_mode = app::PasskeyMode::EnterForCompanion;
            } else {
                // The companion is display-only, so passkey entry is the only valid method.
                ESP_LOGW(TAG, "unexpected pairing action %d; rejecting", ev->passkey.params.action);
                struct ble_sm_io io = {};
                io.action = ev->passkey.params.action;
                io.numcmp_accept = 0;
                ble_sm_inject_io(ev->passkey.conn_handle, &io);
                return 0;
            }
            app::mark(app::kDirtyPasskey);
            return 0;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            struct ble_gap_conn_desc desc;
            if (ble_gap_conn_find(ev->repeat_pairing.conn_handle, &desc) == 0) ble_store_util_delete_peer(&desc.peer_id_addr);
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        case BLE_GAP_EVENT_NOTIFY_RX:
            if (ev->notify_rx.conn_handle == conn.load() && ev->notify_rx.attr_handle == tx_handle) {
                uint8_t buf[256];
                uint16_t n = OS_MBUF_PKTLEN(ev->notify_rx.om);
                if (n <= sizeof(buf) && ble_hs_mbuf_to_flat(ev->notify_rx.om, buf, sizeof(buf), &n) == 0) swlink::on_companion_bytes(buf, n);
            }
            return 0;
        case BLE_GAP_EVENT_MTU:
            cur_mtu.store(ev->mtu.value);
            return 0;
        default:
            return 0;
    }
}

void evaluate() {
    if (conn.load() != BLE_HS_CONN_HANDLE_NONE) return;
    if (!peer.valid) {
        set_state(app::LinkState::NoPeer);
        return;
    }
    if (user_disconnected) {
        set_state(app::LinkState::Disconnected);
        return;
    }
    schedule_retry();
}

void mgr_task(void*) {
    Command c;
    for (;;) {
        TickType_t wait = portMAX_DELAY;
        if (attempt_pending) {
            TickType_t now = xTaskGetTickCount();
            wait = next_attempt > now ? next_attempt - now : 0;
        }
        if (xQueueReceive(cmdq, &c, wait) != pdTRUE) {
            if (attempt_pending) try_connect();
            continue;
        }
        uint16_t h = conn.load();
        switch (c.cmd) {
            case Cmd::Scan: {
                {
                    app::Locked s;
                    s->scan.clear();
                    s->scanning = true;
                }
                app::mark(app::kDirtyScan);
                if (ble_gap_conn_active()) ble_gap_conn_cancel();
                struct ble_gap_disc_params dp = {};
                dp.filter_duplicates = 0;  // need scan responses for names
                dp.passive = 0;
                dp.itvl = 0x50;
                dp.window = 0x30;
                scan_reports = scan_matches = 0;
                int rc = ble_gap_disc(own_addr_type, kScanMs, &dp, gap_event, nullptr);
                ESP_LOGI(TAG, "scan start: rc=%d synced=%d", rc, synced.load());
                if (rc != 0) {
                    app::Locked s;
                    s->scanning = false;
                }
                break;
            }
            case Cmd::StopScan:
                if (ble_gap_disc_active()) ble_gap_disc_cancel();
                {
                    app::Locked s;
                    s->scanning = false;
                }
                app::mark(app::kDirtyScan);
                if (attempt_pending == false) evaluate();
                break;
            case Cmd::Pair:
                if (ble_gap_disc_active()) ble_gap_disc_cancel();
                if (ble_gap_conn_active()) ble_gap_conn_cancel();
                if (h != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
                peer.valid = true;
                memcpy(peer.addr, c.addr, 6);
                peer.addr_type = c.addr_type;
                peer.name = c.name;
                pairing_new = true;
                user_disconnected = false;
                attempt = 0;
                {
                    app::Locked s;
                    s->link.has_peer = true;
                    memcpy(s->link.peer_addr, c.addr, 6);
                    s->link.peer_name = c.name;
                    s->link.last_error.clear();
                }
                if (h == BLE_HS_CONN_HANDLE_NONE) try_connect();  // otherwise after disconnect
                break;
            case Cmd::Disconnect:
                user_disconnected = true;
                attempt_pending = false;
                if (ble_gap_conn_active()) ble_gap_conn_cancel();
                if (h != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
                else set_state(app::LinkState::Disconnected);
                break;
            case Cmd::Reconnect:
                user_disconnected = false;
                attempt = 0;
                if (h == BLE_HS_CONN_HANDLE_NONE) try_connect();
                break;
            case Cmd::Forget: {
                if (h != BLE_HS_CONN_HANDLE_NONE && swlink::ready()) {
                    swlink::send(swp::Unpair{});  // companion drops its bond too, so any watch can pair
                    vTaskDelay(pdMS_TO_TICKS(300));  // let the tx task write it before we disconnect
                }
                attempt_pending = false;
                if (ble_gap_conn_active()) ble_gap_conn_cancel();
                if (peer.valid) {
                    ble_addr_t a = peer_addr();
                    ble_store_util_delete_peer(&a);
                }
                peer = settings::Peer{};
                settings::clear_peer();
                pairing_new = false;
                {
                    app::Locked s;
                    s->link = app::LinkInfo{};
                }
                if (h != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
                set_state(app::LinkState::NoPeer);
                break;
            }
            case Cmd::Passkey:
            case Cmd::CancelPasskey: {
                {
                    app::Locked s;
                    s->passkey_mode = app::PasskeyMode::None;
                }
                app::mark(app::kDirtyPasskey);
                if (h == BLE_HS_CONN_HANDLE_NONE) break;
                if (c.cmd == Cmd::CancelPasskey) {
                    ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
                    user_disconnected = !peer.valid || pairing_new;
                    break;
                }
                struct ble_sm_io pk = {};
                pk.action = BLE_SM_IOACT_INPUT;
                pk.passkey = c.value;
                int rc = ble_sm_inject_io(h, &pk);
                if (rc != 0) ESP_LOGW(TAG, "passkey inject failed: %d", rc);
                break;
            }
            case Cmd::Drop:
                if (h != BLE_HS_CONN_HANDLE_NONE) ble_gap_terminate(h, BLE_ERR_REM_USER_CONN_TERM);
                break;
            case Cmd::Evaluate:
                evaluate();
                break;
        }
    }
}

void on_reset(int reason) { ESP_LOGE(TAG, "NimBLE reset: %d", reason); synced.store(false); }

void on_sync() {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &own_addr_type);
    synced.store(true);
    post(Cmd::Evaluate);
}

void host_task(void*) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

}  // namespace

void init() {
    cmdq = xQueueCreate(8, sizeof(Command));
    peer = settings::peer();
    {
        app::Locked s;
        s->link.has_peer = peer.valid;
        s->link.state = peer.valid ? app::LinkState::Waiting : app::LinkState::NoPeer;
        if (peer.valid) {
            memcpy(s->link.peer_addr, peer.addr, 6);
            s->link.peer_name = peer.name;
        }
    }
    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Keyboard+display: we enter the companion's passkey (kept as-is so existing bonds stay valid).
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_KEYBOARD_DISPLAY;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 1;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    xTaskCreatePinnedToCore(mgr_task, "ble_mgr", 4096, nullptr, 5, nullptr, 0);
    nimble_port_freertos_init(host_task);
}

bool connected() { return conn.load() != BLE_HS_CONN_HANDLE_NONE && tx_cccd != 0; }
uint16_t mtu() { return cur_mtu.load(); }

bool write(const uint8_t* data, size_t len) {
    for (int tries = 0; tries < 200; ++tries) {
        uint16_t h = conn.load();
        if (h == BLE_HS_CONN_HANDLE_NONE || rx_handle == 0) return false;
        int rc = ble_gattc_write_no_rsp_flat(h, rx_handle, data, static_cast<uint16_t>(len));
        if (rc == 0) return true;
        if (rc != BLE_HS_ENOMEM && rc != BLE_HS_EBUSY) return false;
        vTaskDelay(pdMS_TO_TICKS(5));  // controller buffers full: back off briefly
    }
    return false;
}

void start_scan() { post(Cmd::Scan); }
void stop_scan() { post(Cmd::StopScan); }

void pair(const uint8_t addr[6], uint8_t addr_type, const char* name) {
    Command c = {};
    c.cmd = Cmd::Pair;
    memcpy(c.addr, addr, 6);
    c.addr_type = addr_type;
    strlcpy(c.name, name ? name : "Companion", sizeof c.name);
    xQueueSend(cmdq, &c, 0);
}

void disconnect() { post(Cmd::Disconnect); }
void reconnect() { post(Cmd::Reconnect); }
void forget() { post(Cmd::Forget); }
void submit_passkey(uint32_t pk) { post(Cmd::Passkey, pk); }
void cancel_passkey() { post(Cmd::CancelPasskey); }
void drop_link() { post(Cmd::Drop); }

void set_fast(bool on) {
    if (fast_wanted.exchange(on) == on) return;
    uint16_t h = conn.load();
    if (h != BLE_HS_CONN_HANDLE_NONE) apply_conn_params(h);
}

}  // namespace central
