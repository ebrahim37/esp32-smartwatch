// Voice memos: records the headphones' microphone over Bluetooth HFP (the companion is the
// Audio Gateway) into 16-bit mono WAV files under /sdcard/memos (8 kHz CVSD or 16 kHz mSBC,
// whichever the headphones negotiate). Memos play back through the music player (playlist
// "memos").
//
// Recording takes the headphones for the duration: music is paused (AirPods can't stream A2DP
// and run a hands-free audio link at once). Afterwards the whole Bluetooth connection is closed:
// on AirPods, A2DP over a connection that carried the hands-free audio stays congested.
// TODO(HARDWARE): recording verified on AirPods; playback after recording still being verified.
#pragma once

#include <string>
#include <vector>

#include "music/library.hpp"
#include "swp/messages.hpp"

namespace music::memos {

constexpr const char* kPlaylistId = "memos";

void init();  // after bt_core::init()
void command(const swp::MemoCommand& c);
swp::MemoStatus status();
swp::MemoList list();  // newest first

// Headphone button (AVRCP) presses while a memo records, or just after: with music connected the
// headphones send the press as play/pause instead of ending voice recognition. Returns true if
// the press was taken (it stops the recording, or is the late duplicate of the press that did).
bool take_button();

// Playlist view of the memos for the player (same order as list()).
uint16_t count();
bool track_at(uint16_t index, library::Track& out);

}  // namespace music::memos
