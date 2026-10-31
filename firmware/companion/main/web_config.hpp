// Plain-HTTP settings page on port 80 of the companion's LAN address.
//
// Served only while the watch has opened it (watch Settings → Web configuration, 5 min);
// the server stops when that window ends. Traffic is unencrypted, so use it on a trusted
// network only. Secret values are write-only, exactly as over BLE.
#pragma once

#include <cstdint>

#include "swp/messages.hpp"

namespace web_config {

void start();
void open(uint16_t seconds);  // 0 closes the page
swp::WebConfigStatus status();

}  // namespace web_config
