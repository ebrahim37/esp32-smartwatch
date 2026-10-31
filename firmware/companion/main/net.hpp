// Internet connectivity manager.
//
// Providers in priority order:
//   1. Home Wi-Fi (configured SSID). Whenever Wi-Fi is connected, it is the only data path.
//   2. TODO(LTE): EG912U cellular modem (see lte.hpp). Absent on this prototype.
//   3. TODO(PHONE): a phone app relaying internet over BLE (future, watch-side provider).
//
// While Wi-Fi is configured but not connected, the manager scans for the SSID with an
// exponential backoff (10 s .. 5 min) instead of scanning continuously.
#pragma once

#include <cstdint>
#include <string>

#include "swp/messages.hpp"

namespace net {

void start();
swp::WifiStatus status();
bool online();                // some provider has an IP and DNS
swp::NetProvider provider();
void reconfigure();           // apply new SSID/password immediately
bool time_synced();
void set_server_ok(bool ok);  // updated by the cloud client

using Listener = void (*)(bool online);
void add_listener(Listener l);

}  // namespace net
