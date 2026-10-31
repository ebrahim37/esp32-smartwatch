#include "music/library.hpp"

#include <cstring>
#include <mutex>

#include "cJSON.h"
#include "esp_log.h"
#include "storage.hpp"

namespace music::library {
namespace {

constexpr const char* TAG = "library";
constexpr const char* kPath = "/sdcard/music/library.json";
std::mutex mu;
std::vector<Playlist> lists;

const Playlist* find(const std::string& id) {
    for (auto& p : lists)
        if (p.id == id) return &p;
    return nullptr;
}

std::string jstr(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

}  // namespace

void load() {
    std::vector<uint8_t> raw;
    if (!storage::read_file(kPath, raw, 2 * 1024 * 1024)) return;
    raw.push_back(0);
    cJSON* root = cJSON_Parse(reinterpret_cast<char*>(raw.data()));
    if (!root) {
        ESP_LOGW(TAG, "library.json unreadable; will be rebuilt by the next sync");
        return;
    }
    std::vector<Playlist> out;
    const cJSON* pl;
    cJSON_ArrayForEach(pl, cJSON_GetObjectItemCaseSensitive(root, "playlists")) {
        Playlist p;
        p.id = jstr(pl, "id");
        p.name = jstr(pl, "name");
        const cJSON* t;
        cJSON_ArrayForEach(t, cJSON_GetObjectItemCaseSensitive(pl, "tracks")) {
            Track tr;
            tr.id = jstr(t, "id");
            tr.title = jstr(t, "title");
            tr.artist = jstr(t, "artist");
            tr.album = jstr(t, "album");
            tr.suffix = jstr(t, "suffix");
            const cJSON* d = cJSON_GetObjectItemCaseSensitive(t, "duration");
            tr.duration_s = cJSON_IsNumber(d) ? static_cast<uint32_t>(d->valuedouble) : 0;
            const cJSON* s = cJSON_GetObjectItemCaseSensitive(t, "size");
            tr.size = cJSON_IsNumber(s) ? static_cast<uint64_t>(s->valuedouble) : 0;
            tr.available = storage::file_size(path_for(tr.id, tr.suffix)) == tr.size && tr.size > 0;
            p.tracks.push_back(std::move(tr));
        }
        out.push_back(std::move(p));
    }
    cJSON_Delete(root);
    std::lock_guard<std::mutex> g(mu);
    lists = std::move(out);
    ESP_LOGI(TAG, "loaded %u playlist(s)", static_cast<unsigned>(lists.size()));
}

bool save() {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "playlists");
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& p : lists) {
            cJSON* o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "id", p.id.c_str());
            cJSON_AddStringToObject(o, "name", p.name.c_str());
            cJSON* ta = cJSON_AddArrayToObject(o, "tracks");
            for (auto& t : p.tracks) {
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
            cJSON_AddItemToArray(arr, o);
        }
    }
    char* s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return false;
    bool ok = storage::write_file_atomic(kPath, reinterpret_cast<uint8_t*>(s), strlen(s));
    cJSON_free(s);
    return ok;
}

void replace(Playlist p) {
    std::lock_guard<std::mutex> g(mu);
    for (auto& t : p.tracks) t.available = t.size > 0 && storage::file_size(path_for(t.id, t.suffix)) == t.size;
    // TODO(PROTOTYPE): a single synchronised playlist replaces the whole set.
    lists.clear();
    lists.push_back(std::move(p));
}

std::vector<swp::PlaylistInfo> playlists() {
    std::lock_guard<std::mutex> g(mu);
    std::vector<swp::PlaylistInfo> out;
    for (auto& p : lists) {
        swp::PlaylistInfo i;
        i.id = p.id;
        i.name = p.name;
        i.track_count = static_cast<uint16_t>(p.tracks.size());
        bool all = !p.tracks.empty();
        for (auto& t : p.tracks) {
            i.duration_s += t.duration_s;
            all &= t.available;
        }
        i.synced = all;
        out.push_back(i);
    }
    return out;
}

swp::PlaylistTracks page(const std::string& id, uint16_t offset, uint16_t count) {
    std::lock_guard<std::mutex> g(mu);
    swp::PlaylistTracks out;
    out.playlist_id = id;
    out.offset = offset;
    const Playlist* p = find(id);
    if (!p) return out;
    out.total = static_cast<uint16_t>(p->tracks.size());
    count = std::min<uint16_t>(count, 12);
    for (uint16_t i = offset; i < p->tracks.size() && out.tracks.size() < count; ++i) {
        const Track& t = p->tracks[i];
        swp::TrackInfo ti;
        ti.id = t.id;
        ti.title = t.title;
        ti.artist = t.artist;
        ti.album = t.album;
        ti.duration_s = t.duration_s;
        ti.index = i;
        ti.available = t.available;
        out.tracks.push_back(std::move(ti));
    }
    return out;
}

bool track_at(const std::string& id, uint16_t index, Track& out) {
    std::lock_guard<std::mutex> g(mu);
    const Playlist* p = find(id);
    if (!p || index >= p->tracks.size()) return false;
    out = p->tracks[index];
    return true;
}

uint16_t size(const std::string& id) {
    std::lock_guard<std::mutex> g(mu);
    const Playlist* p = find(id);
    return p ? static_cast<uint16_t>(p->tracks.size()) : 0;
}

std::string path_for(const std::string& id, const std::string& suffix) {
    return std::string(storage::kMusicDir) + "/" + id + "." + suffix;
}

std::string first_playlist_id() {
    std::lock_guard<std::mutex> g(mu);
    return lists.empty() ? "" : lists.front().id;
}

}  // namespace music::library
