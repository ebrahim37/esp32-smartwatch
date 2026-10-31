// Music playback: SD file -> esp_audio_simple_dec (container parse + decode) -> PCM
// conversion (44.1 kHz/16-bit/stereo) -> lock-free ring buffer -> A2DP source callback.
//
// The decoder runs on its own high-priority task pinned to core 1 (Wi-Fi/BLE controllers and
// network tasks sit on core 0), with a ~0.7 s PCM ring in PSRAM to ride out SD latency.
// The A2DP callback never blocks or allocates.
#pragma once

#include "swp/messages.hpp"

namespace music::player {

void init();
void command(const swp::PlaybackCommand& c);
swp::PlaybackStatus status();
bool active();     // playing or buffering: background sync/transfers should yield
bool streaming();  // A2DP media channel is running
void alert_tone(uint8_t repeats);

}  // namespace music::player
