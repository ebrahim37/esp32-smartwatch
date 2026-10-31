// NimBLE peripheral role exposing the SWP service for the Web Bluetooth configuration app.
// Advertising is off by default and only enabled for a short window from Settings.
// All SWP characteristics require an encrypted, MITM-authenticated (passkey) link.
#pragma once

#include <cstddef>
#include <cstdint>

namespace peripheral {

// Registers the GATT service. Must run before the NimBLE host starts.
void register_services();
void on_sync();  // host synced

void start_advertising(uint32_t seconds);
void stop_advertising();
bool connected();
uint16_t mtu();
bool notify(const uint8_t* data, size_t len);
void confirm_numeric(bool accept);

}  // namespace peripheral
