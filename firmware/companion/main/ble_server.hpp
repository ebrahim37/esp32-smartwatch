// BLE GATT server (Bluedroid) exposing the SWP service to the watch.
//
// Security: LE Secure Connections with MITM protection and bonding. The companion has no
// display, so it uses a static 6-digit passkey (printed on the serial console at boot and
// stored in the cfg partition). New pairings are only accepted inside a pairing window
// (CONFIG_COMPANION_PAIRING_WINDOW_S after boot, re-opened by pressing BOOT); bonded peers
// reconnect at any time. Every SWP characteristic requires an authenticated link.
#pragma once

#include <cstdint>
#include <vector>

#include "swp/messages.hpp"

namespace ble_server {

struct Session {
    uint16_t conn_id = 0xFFFF;
    uint8_t bda[6] = {};
    bool authenticated = false;
    bool notify = false;
    uint16_t mtu = 23;
    uint8_t role = 0;  // swp::Role from Hello (0 until handshake)
};

void init();  // after bt_core::init()

bool send(uint16_t conn_id, uint8_t type, std::vector<uint8_t>&& payload);
template <class M>
bool send(uint16_t conn_id, const M& m) {
    return send(conn_id, static_cast<uint8_t>(M::kType), swp::encode_payload(m));
}
// To every handshaken watch session.
bool to_watch(uint8_t type, std::vector<uint8_t>&& payload);
template <class M>
bool to_watch(const M& m) {
    return to_watch(static_cast<uint8_t>(M::kType), swp::encode_payload(m));
}
bool watch_connected();
uint16_t watch_conn();
uint16_t mtu(uint16_t conn_id);
// Number of frames waiting for this connection (used for transfer backpressure).
int queued(uint16_t conn_id);
void set_role(uint16_t conn_id, uint8_t role);
uint8_t role(uint16_t conn_id);
void open_pairing_window(uint32_t seconds);
bool pairing_window_open();

}  // namespace ble_server
