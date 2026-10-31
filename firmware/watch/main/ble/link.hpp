// SWP session layer on the watch.
//
// Two transports feed this layer:
//   * the NimBLE central connection to the companion (ble/central.cpp), and
//   * an optional authenticated peripheral connection from the Web Bluetooth config app
//     (ble/peripheral.cpp).
// Outgoing frames are queued and written by a dedicated task, so callers (UI, services)
// never block on the radio.
#pragma once

#include <cstdint>
#include <vector>

#include "swp/messages.hpp"

namespace swlink {

enum class Target : uint8_t { Companion, Web };

void init();

bool enqueue(Target t, uint8_t type, std::vector<uint8_t>&& payload);

template <class M>
bool send(const M& m) {
    return enqueue(Target::Companion, static_cast<uint8_t>(M::kType), swp::encode_payload(m));
}
template <class M>
bool send_web(const M& m) {
    return enqueue(Target::Web, static_cast<uint8_t>(M::kType), swp::encode_payload(m));
}

bool ready();  // companion handshake complete

// Transport callbacks.
void on_companion_ready();  // GATT subscribed: start the handshake
void on_companion_down();
void on_companion_bytes(const uint8_t* data, size_t len);
void on_web_connected();
void on_web_down();
void on_web_bytes(const uint8_t* data, size_t len);

// Convenience requests used by the UI.
void request_playlists();
void request_tracks(const std::string& playlist_id, uint16_t offset);
void playback(swp::PlaybackCmd cmd, uint32_t arg = 0, const std::string& str = "");
void send_alert_tone();
void request_status();

}  // namespace swlink
