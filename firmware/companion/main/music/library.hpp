// Offline music library: track metadata and playlists synchronised from Navidrome, stored as
// JSON on the SD card (/sdcard/music/library.json, rewritten after a sync).
//
// One table of tracks (every Navidrome playlist plus the rest of the library, as far as the
// music quota allows); playlists index into it. "All Songs" (kAllSongsId) is a virtual
// playlist of every downloaded track, sorted by title; "search:<text>" (kSearchPrefix) is a
// virtual playlist of the downloaded tracks whose title or artist contains <text>.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "swp/messages.hpp"

namespace music::library {

constexpr const char* kAllSongsId = "all";
constexpr const char* kSearchPrefix = "search:";

struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string suffix;
    uint32_t duration_s = 0;
    uint64_t size = 0;
    bool available = false;  // complete file present on the card
    std::string path;        // file location when not in the music directory (voice memos)
};

struct Playlist {
    std::string id;
    std::string name;
    std::vector<uint32_t> tracks;  // indices into the track table, playlist order
    uint32_t duration_s = 0;       // server total (exact; per-track durations are rounded)
};

void load();
bool save();
// After a successful metadata sync. `available` must already be set on each track.
void replace(std::vector<Track> tracks, std::vector<Playlist> lists);
void set_available(const std::string& track_id, uint64_t size);  // a download finished
std::vector<swp::PlaylistInfo> playlists();  // real playlists (not the virtual ones)
swp::PlaylistTracks page(const std::string& playlist_id, uint16_t offset, uint16_t count);
bool track_at(const std::string& playlist_id, uint16_t index, Track& out);
uint16_t size(const std::string& playlist_id);
std::string path_for(const std::string& id, const std::string& suffix);

}  // namespace music::library
