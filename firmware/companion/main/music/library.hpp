// Offline music library: playlist metadata synchronised from Navidrome, stored as JSON on
// the SD card (/sdcard/music/library.json, rewritten only after a sync).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "swp/messages.hpp"

namespace music::library {

struct Track {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    std::string suffix;
    uint32_t duration_s = 0;
    uint64_t size = 0;
    bool available = false;  // complete file present on the card
};

struct Playlist {
    std::string id;
    std::string name;
    std::vector<Track> tracks;  // playlist order
};

void load();
bool save();
void replace(Playlist p);  // after a successful sync
std::vector<swp::PlaylistInfo> playlists();
swp::PlaylistTracks page(const std::string& playlist_id, uint16_t offset, uint16_t count);
bool track_at(const std::string& playlist_id, uint16_t index, Track& out);
uint16_t size(const std::string& playlist_id);
std::string path_for(const std::string& id, const std::string& suffix);
std::string first_playlist_id();

}  // namespace music::library
