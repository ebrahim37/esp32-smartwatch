// Navidrome (Subsonic API) synchronisation of the music library to the SD card.
//
// Every playlist's tracks are synced first, then the rest of the library (search3 with an
// empty query) until the music quota (ccore::kMusicQuota) is used. Tracks are fetched
// transcoded to 128 kbps MP3 into /sdcard/music/.tmp/<id>.mp3.part and renamed into place only
// when complete. Sync runs only on home Wi-Fi and pauses while music plays so SD and radio
// bandwidth go to audio first.
#pragma once

#include <cstdint>

#include "swp/messages.hpp"

namespace music::navidrome {

void start();
void trigger();  // sync soon (config change, user request)
swp::SyncState state();
uint16_t done();
uint16_t total();

}  // namespace music::navidrome
