// Bluetooth Classic A2DP source (SBC to the headphones) with AVRCP for remote controls and
// absolute volume, plus inquiry-based discovery and pairing of audio sinks.
//
// TODO(HARDWARE): written against the generic A2DP/AVRCP profiles. AirPods-specific
// behaviour (pairing-mode handshake, automatic ear detection pausing, absolute volume quirks,
// multipoint hand-off to an iPhone) has not been verified on real AirPods yet.
#pragma once

#include <cstdint>

#include "swp/messages.hpp"

namespace music::a2dp {

// Producer of 44.1 kHz, 16-bit, stereo interleaved PCM. Must fill `len` bytes and return
// the number written (fill silence on underrun). Runs in the Bluetooth task: no blocking.
using PcmSource = int32_t (*)(uint8_t* buf, int32_t len);

void init(PcmSource src);  // after bt_core::init()

void scan(uint8_t seconds);
void connect(const uint8_t bda[6]);
void disconnect();
void forget(const uint8_t bda[6]);

bool connected();
bool streaming();
void start_stream();    // request the media channel (no-op if already started)
void suspend_stream();
void set_volume(uint8_t v);  // 0..127, AVRCP absolute volume if supported
uint8_t volume();

swp::AudioStatus status();
swp::AudioDevices devices();

// Remote control commands from the headphones (play/pause/next/prev).
using RemoteCmd = void (*)(swp::PlaybackCmd cmd);
void set_remote_handler(RemoteCmd h);

}  // namespace music::a2dp
