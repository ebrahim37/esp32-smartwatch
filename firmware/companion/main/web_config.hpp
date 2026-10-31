// Plain-HTTP configuration page on port 80 of the companion's LAN address.
//
// Only served while the Bluetooth pairing window is open (after boot, or after pressing
// BOOT); the server is stopped when the window closes. Traffic is unencrypted, so use it on
// a trusted network only. Secret values are write-only, exactly as over BLE.
#pragma once

namespace web_config {

void start();

}  // namespace web_config
