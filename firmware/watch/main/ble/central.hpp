// NimBLE central role: discovery, bonding and automatic reconnection to the companion.
#pragma once

#include <cstddef>
#include <cstdint>

namespace central {

// Initialises NimBLE and starts the connection manager.
void init();

bool connected();     // GATT link up (may still be handshaking)
uint16_t mtu();
bool write(const uint8_t* data, size_t len);  // write-without-response with flow control

// User actions (non-blocking; processed by the connection manager task).
void start_scan();
void stop_scan();
void pair(const uint8_t addr[6], uint8_t addr_type, const char* name);
void disconnect();   // stays disconnected until reconnect()
void reconnect();
void forget();
void submit_passkey(uint32_t passkey);
void cancel_passkey();
void drop_link();    // transient drop (health check); auto-reconnect continues

}  // namespace central
