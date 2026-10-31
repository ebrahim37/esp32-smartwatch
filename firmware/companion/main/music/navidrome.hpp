// Navidrome (Subsonic API) synchronisation of the configured playlist to the SD card.
//
// TODO(PROTOTYPE): exactly one playlist (config key `nd_playlist`, default
// CONFIG_PROTOTYPE_PLAYLIST_ID) is synchronised. Production will sync a selected set.
//
// Files are downloaded with `download` (original file, no transcoding) to
// /sdcard/music/.tmp/<id>.part, resumed with HTTP Range after interruptions, and renamed into
// place only when complete. Sync runs only on home Wi-Fi and pauses while music plays so
// SD and radio bandwidth go to audio first.
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
