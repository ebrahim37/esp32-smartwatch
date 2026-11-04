#include "music/library.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <strings.h>
#include <mutex>
#include <unordered_map>

#include "json.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "storage.hpp"

namespace music::library {
namespace {

constexpr const char* TAG = "library";
constexpr const char* kPath = "/sdcard/music/library.json";
std::mutex mu;
std::vector<Track> tracks;
std::vector<Playlist> lists;
std::vector<uint32_t> all_songs;  // downloaded tracks, sorted by title
std::string search_id;            // cached last search ("search:<text>") and its matches
std::vector<uint32_t> search_hits;

// Case-folded for matching: ASCII lower case, and the Arabic Yeh/Kaf written as their Persian
// forms (both spellings are common in names).
std::string fold(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = in[i];
        if (c == 0xD9 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x8A) {
            s += "\xDB\x8C";  // U+064A -> U+06CC
            ++i;
        } else if (c == 0xD9 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x83) {
            s += "\xDA\xA9";  // U+0643 -> U+06A9
            ++i;
        } else {
            s += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
        }
    }
    return s;
}

// Track indices of a playlist (or of All Songs); nullptr if unknown. Caller holds mu.
const std::vector<uint32_t>* indices(const std::string& id) {
    if (id == kAllSongsId) return &all_songs;
    if (id.rfind(kSearchPrefix, 0) == 0) {
        if (id != search_id) {
            search_id = id;
            search_hits.clear();
            std::string q = fold(id.substr(strlen(kSearchPrefix)));
            for (uint32_t i : all_songs)
                if (fold(tracks[i].title).find(q) != std::string::npos || fold(tracks[i].artist).find(q) != std::string::npos) search_hits.push_back(i);
        }
        return &search_hits;
    }
    for (auto& p : lists)
        if (p.id == id) return &p.tracks;
    return nullptr;
}

void rebuild_all_songs() {
    search_id.clear();
    all_songs.clear();
    for (uint32_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].available) all_songs.push_back(i);
    std::sort(all_songs.begin(), all_songs.end(), [](uint32_t a, uint32_t b) { return strcasecmp(tracks[a].title.c_str(), tracks[b].title.c_str()) < 0; });
}

uint64_t jnum(const cJSON* o, const char* k) {
    double v = json::number(o, k);
    return v > 0 ? static_cast<uint64_t>(v) : 0;
}

}  // namespace

void load() {
    int64_t t0 = esp_timer_get_time();
    std::vector<uint8_t> raw;
    if (!storage::read_file(kPath, raw, 16 * 1024 * 1024)) {
        ESP_LOGW(TAG, "no %s yet; the next sync builds it", kPath);
        return;
    }
    // Downloaded files and their sizes, from one directory listing (storage::list_dir).
    std::unordered_map<std::string, uint64_t> present;
    storage::list_dir(storage::kMusicDir, [&](const char* name, uint64_t size, bool is_dir) {
        if (!is_dir) present.emplace(name, size);
    });
    cJSON* root = cJSON_ParseWithLength(reinterpret_cast<const char*>(raw.data()), raw.size());
    if (!root || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "tracks"))) {
        ESP_LOGW(TAG, "library.json unreadable or old format; will be rebuilt by the next sync");
        cJSON_Delete(root);
        return;
    }
    std::vector<Track> ts;
    const cJSON* t;
    cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(root, "tracks")) {
        Track tr;
        tr.id = json::string(t, "id");
        tr.title = json::string(t, "title");
        tr.artist = json::string(t, "artist");
        tr.album = json::string(t, "album");
        tr.suffix = json::string(t, "suffix");
        tr.duration_s = static_cast<uint32_t>(jnum(t, "duration"));
        tr.size = jnum(t, "size");
        auto f = present.find(tr.id + "." + tr.suffix);
        tr.available = tr.size > 0 && f != present.end() && f->second == tr.size;
        ts.push_back(std::move(tr));
    }
    std::vector<Playlist> ls;
    const cJSON* pl;
    cJSON_ArrayForEach(pl, cJSON_GetObjectItemCaseSensitive(root, "playlists")) {
        Playlist p;
        p.id = json::string(pl, "id");
        p.name = json::string(pl, "name");
        p.duration_s = static_cast<uint32_t>(jnum(pl, "duration"));
        const cJSON* i;
        cJSON_ArrayForEach(i, cJSON_GetObjectItemCaseSensitive(pl, "tracks")) {
            if (cJSON_IsNumber(i) && i->valuedouble >= 0 && i->valuedouble < ts.size()) p.tracks.push_back(static_cast<uint32_t>(i->valuedouble));
        }
        ls.push_back(std::move(p));
    }
    cJSON_Delete(root);
    std::lock_guard<std::mutex> g(mu);
    tracks = std::move(ts);
    lists = std::move(ls);
    rebuild_all_songs();
    ESP_LOGI(TAG, "loaded %u tracks (%u downloaded), %u playlist(s) in %lld ms", (unsigned)tracks.size(), (unsigned)all_songs.size(),
             (unsigned)lists.size(), (esp_timer_get_time() - t0) / 1000);
}

bool save() {
    cJSON* root = cJSON_CreateObject();
    cJSON* ta = cJSON_AddArrayToObject(root, "tracks");
    cJSON* pa = cJSON_AddArrayToObject(root, "playlists");
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& t : tracks) {
            cJSON* to = cJSON_CreateObject();
            cJSON_AddStringToObject(to, "id", t.id.c_str());
            cJSON_AddStringToObject(to, "title", t.title.c_str());
            cJSON_AddStringToObject(to, "artist", t.artist.c_str());
            cJSON_AddStringToObject(to, "album", t.album.c_str());
            cJSON_AddStringToObject(to, "suffix", t.suffix.c_str());
            cJSON_AddNumberToObject(to, "duration", t.duration_s);
            cJSON_AddNumberToObject(to, "size", static_cast<double>(t.size));
            cJSON_AddItemToArray(ta, to);
        }
        for (auto& p : lists) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", p.id.c_str());
            cJSON_AddStringToObject(o, "name", p.name.c_str());
            cJSON_AddNumberToObject(o, "duration", p.duration_s);
            cJSON* ia = cJSON_AddArrayToObject(o, "tracks");
            for (uint32_t i : p.tracks) cJSON_AddItemToArray(ia, cJSON_CreateNumber(i));
            cJSON_AddItemToArray(pa, o);
        }
    }
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return false;
    bool ok = storage::write_file_atomic(kPath, reinterpret_cast<uint8_t*>(s), strlen(s));
    cJSON_free(s);
    return ok;
}

void replace(std::vector<Track> ts, std::vector<Playlist> ls) {
    std::lock_guard<std::mutex> g(mu);
    tracks = std::move(ts);
    lists = std::move(ls);
    rebuild_all_songs();
}

void set_available(const std::string& id, uint64_t size) {
    std::lock_guard<std::mutex> g(mu);
    for (auto& t : tracks)
        if (t.id == id) {
            t.size = size;
            t.available = size > 0;
        }
    rebuild_all_songs();
}

std::vector<swp::PlaylistInfo> playlists() {
    std::lock_guard<std::mutex> g(mu);
    std::vector<swp::PlaylistInfo> out;
    auto info = [&](const std::string& id, const std::string& name, const std::vector<uint32_t>& idx, uint32_t total_s) {
        swp::PlaylistInfo i;
        i.id = id;
        i.name = name;
        i.track_count = static_cast<uint16_t>(std::min<size_t>(idx.size(), UINT16_MAX));
        bool all = !idx.empty();
        for (uint32_t k : idx) {
            i.duration_s += tracks[k].duration_s;
            all &= tracks[k].available;
        }
        i.synced = all;
        if (total_s) i.duration_s = total_s;
        out.push_back(i);
    };
    for (auto& p : lists) info(p.id, p.name, p.tracks, p.duration_s);
    return out;
}

swp::PlaylistTracks page(const std::string& id, uint16_t offset, uint16_t count) {
    std::lock_guard<std::mutex> g(mu);
    swp::PlaylistTracks out;
    out.playlist_id = id;
    out.offset = offset;
    const auto* idx = indices(id);
    if (!idx) return out;
    out.total = static_cast<uint16_t>(std::min<size_t>(idx->size(), UINT16_MAX));
    count = std::min<uint16_t>(count, 12);
    for (size_t i = offset; i < out.total && out.tracks.size() < count; ++i) {
        const Track& t = tracks[(*idx)[i]];
        swp::TrackInfo ti;
        ti.id = t.id;
        ti.title = t.title;
        ti.artist = t.artist;
        ti.album = t.album;
        ti.duration_s = t.duration_s;
        ti.index = static_cast<uint16_t>(i);
        ti.available = t.available;
        out.tracks.push_back(std::move(ti));
    }
    return out;
}

bool track_at(const std::string& id, uint16_t index, Track& out) {
    std::lock_guard<std::mutex> g(mu);
    const auto* idx = indices(id);
    if (!idx || index >= idx->size()) return false;
    out = tracks[(*idx)[index]];
    return true;
}

uint16_t size(const std::string& id) {
    std::lock_guard<std::mutex> g(mu);
    const auto* idx = indices(id);
    return idx ? static_cast<uint16_t>(std::min<size_t>(idx->size(), UINT16_MAX)) : 0;
}

std::string path_for(const std::string& id, const std::string& suffix) {
    return std::string(storage::kMusicDir) + "/" + id + "." + suffix;
}

}  // namespace music::library
