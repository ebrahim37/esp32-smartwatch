#include "music/navidrome.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <unordered_map>

#include "cJSON.h"
#include "ccore/quota.hpp"
#include "ccore/subsonic.hpp"
#include "ccore/sync_plan.hpp"
#include "config.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "http.hpp"
#include "music/library.hpp"
#include "music/player.hpp"
#include "net.hpp"
#include "storage.hpp"

namespace music::navidrome {
namespace {

constexpr const char* TAG = "navidrome";
constexpr TickType_t kPeriod = pdMS_TO_TICKS(6 * 3600 * 1000);  // re-check every 6 h on Wi-Fi
constexpr EventBits_t kTrigger = 1;
// Tracks are fetched transcoded (`stream`), not as originals (`download`): a lossless track is
// ~25 MB, the same track at 128 kbps MP3 ~4 MB, which is what the companion's ~200 KB/s Wi-Fi
// path can sync quickly. The headphones get SBC anyway. Transcoded sizes are only known once
// downloaded, so these files can't be resumed; an interrupted track starts over.
constexpr const char* kFormat = "mp3";
constexpr unsigned kKbps = 128;

EventGroupHandle_t ev;
std::atomic<uint8_t> st{static_cast<uint8_t>(swp::SyncState::Idle)};
std::atomic<uint16_t> n_done{0}, n_total{0};

std::string salt() {
    char b[17];
    snprintf(b, sizeof b, "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
    return b;
}

ccore::SubsonicAuth auth() {
    ccore::SubsonicAuth a;
    a.user = config::get(config::kNdUser);
    a.password = config::get(config::kNdPass);
    return a;
}


std::string url(const char* method, const std::string& extra) {
    return ccore::subsonic_url(config::get(config::kNdUrl), method, auth(), salt(), extra);
}

cJSON* api(const char* method, const std::string& extra, std::vector<uint8_t>& body) {
    auto r = http::get(url(method, extra), {}, body, 1024 * 1024, true);
    if (!r.ok()) return nullptr;
    body.push_back(0);
    cJSON* root = cJSON_Parse(reinterpret_cast<char*>(body.data()));
    const cJSON* resp = cJSON_GetObjectItemCaseSensitive(root, "subsonic-response");
    const cJSON* status = cJSON_GetObjectItemCaseSensitive(resp, "status");
    if (!cJSON_IsString(status) || strcmp(status->valuestring, "ok") != 0) {
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(resp, "error");
        const cJSON* msg = cJSON_GetObjectItemCaseSensitive(err, "message");
        ESP_LOGW(TAG, "%s failed: %s", method, cJSON_IsString(msg) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return nullptr;
    }
    return root;
}

std::string jstr(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) return v->valuestring;
    if (cJSON_IsNumber(v)) return std::to_string(static_cast<long long>(v->valuedouble));
    return "";
}

// Subsonic song object (playlist "entry" / search3 "song") -> track; false for unusable ids.
bool parse_song(const cJSON* e, library::Track& t) {
    t.id = jstr(e, "id");
    if (!ccore::safe_id(t.id)) {
        ESP_LOGW(TAG, "skipping track with unsafe id %s", t.id.c_str());
        return false;
    }
    t.title = jstr(e, "title");
    t.artist = jstr(e, "artist");
    t.album = jstr(e, "album");
    const cJSON* d = cJSON_GetObjectItemCaseSensitive(e, "duration");
    t.duration_s = cJSON_IsNumber(d) ? static_cast<uint32_t>(d->valuedouble) : 0;
    t.suffix = kFormat;  // downloaded transcoded
    t.size = 0;          // filled in from the card
    return true;
}

uint64_t budget_for(const library::Track& t) { return (t.duration_s + 5) * kKbps * 125ull; }

std::vector<ccore::LocalFile> scan_local() {
    std::vector<ccore::LocalFile> out;
    auto scan = [&](const char* dir, bool partial) {
        DIR* d = opendir(dir);
        if (!d) return;
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.empty() || n[0] == '.' || n == "library.json") continue;
            size_t dot = n.find('.');
            if (dot == std::string::npos) continue;
            ccore::LocalFile f;
            f.id = n.substr(0, dot);
            f.suffix = n.substr(dot + 1);
            if (partial) {
                if (f.suffix.size() < 5 || f.suffix.compare(f.suffix.size() - 5, 5, ".part") != 0) continue;
                f.suffix.resize(f.suffix.size() - 5);
            }
            f.partial = partial;
            f.size = storage::file_size(std::string(dir) + "/" + n);
            if (ccore::safe_id(f.id)) out.push_back(f);
        }
        closedir(d);
    };
    scan(storage::kMusicDir, false);
    scan(storage::kMusicTmpDir, true);
    return out;
}

bool should_pause() { return !net::online() || player::active(); }

// Everything on the server, in download priority order: every playlist's tracks first, then
// the rest of the library until the music quota is covered. Fails (and the caller deletes
// nothing) if any metadata request fails.
bool fetch_catalogue(std::vector<library::Track>& table, std::vector<library::Playlist>& lists) {
    std::unordered_map<std::string, uint32_t> by_id;
    uint64_t budget = 0;
    auto add = [&](library::Track&& t) -> uint32_t {
        auto it = by_id.find(t.id);
        if (it != by_id.end()) return it->second;
        budget += budget_for(t);
        uint32_t i = static_cast<uint32_t>(table.size());
        by_id.emplace(t.id, i);
        table.push_back(std::move(t));
        return i;
    };

    std::vector<uint8_t> body;
    cJSON* root = api("getPlaylists", "", body);
    if (!root) return false;
    const cJSON* pls = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "subsonic-response"), "playlists"), "playlist");
    const cJSON* p;
    cJSON_ArrayForEach(p, pls) {
        library::Playlist pl;
        pl.id = jstr(p, "id");
        pl.name = jstr(p, "name");
        const cJSON* d = cJSON_GetObjectItemCaseSensitive(p, "duration");
        pl.duration_s = cJSON_IsNumber(d) && d->valuedouble > 0 ? static_cast<uint32_t>(d->valuedouble) : 0;
        if (!pl.id.empty() && pl.id != library::kAllSongsId) lists.push_back(std::move(pl));
    }
    cJSON_Delete(root);

    for (auto& pl : lists) {
        body.clear();
        root = api("getPlaylist", "id=" + ccore::url_encode(pl.id), body);
        if (!root) return false;
        const cJSON* e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "subsonic-response"), "playlist"), "entry")) {
            library::Track t;
            if (parse_song(e, t)) pl.tracks.push_back(add(std::move(t)));
        }
        cJSON_Delete(root);
    }

    // The rest of the library: search3 with an empty query lists every song (Navidrome).
    constexpr unsigned kPage = 200;
    for (unsigned offset = 0; budget < ccore::kMusicQuota; offset += kPage) {
        body.clear();
        char q[96];
        snprintf(q, sizeof q, "query=&artistCount=0&albumCount=0&songCount=%u&songOffset=%u", kPage, offset);
        root = api("search3", q, body);
        if (!root) return false;
        unsigned n = 0;
        const cJSON* e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "subsonic-response"), "searchResult3"), "song")) {
            ++n;
            if (budget >= ccore::kMusicQuota) break;
            library::Track t;
            if (parse_song(e, t)) add(std::move(t));
        }
        cJSON_Delete(root);
        if (n < kPage) break;
    }
    return true;
}

bool sync_once() {
    if (config::get(config::kNdUrl).empty() || config::get(config::kNdUser).empty()) {
        st.store(static_cast<uint8_t>(swp::SyncState::NotConfigured));
        return false;
    }
    if (!storage::mounted()) return false;
    st.store(static_cast<uint8_t>(swp::SyncState::Running));

    std::vector<library::Track> table;
    std::vector<library::Playlist> lists;
    if (!fetch_catalogue(table, lists)) {
        st.store(static_cast<uint8_t>(swp::SyncState::Failed));
        return false;  // never delete anything on a failed metadata fetch
    }
    std::vector<ccore::RemoteTrack> remote;
    remote.reserve(table.size());
    for (auto& t : table) remote.push_back({t.id, t.suffix, 0, budget_for(t)});

    auto local = scan_local();
    for (auto& t : table)
        for (auto& f : local)
            if (f.id == t.id && !f.partial && f.suffix == t.suffix) {
                t.size = f.size;
                t.available = f.size > 0;
            }
    // Several files per id (an original from before transcoding, a .part): the plan should
    // see the transcoded one.
    std::stable_partition(local.begin(), local.end(), [](const ccore::LocalFile& f) { return f.partial || f.suffix != kFormat; });
    auto plan = ccore::plan_sync(remote, local, ccore::kMusicQuota);
    ESP_LOGI(TAG, "%u playlist(s), %u tracks: keep %u, download %u, resume %u, remove %u, over quota %u", (unsigned)lists.size(),
             (unsigned)remote.size(), (unsigned)plan.keep.size(), (unsigned)plan.download.size(), (unsigned)plan.resume.size(),
             (unsigned)plan.remove.size(), (unsigned)plan.over_quota.size());

    // Remove files no longer on the server or over quota (the catalogue fetch succeeded, so
    // this is safe).
    for (auto& id : plan.remove)
        for (auto& f : local)
            if (f.id == id) {
                std::string p = f.partial ? std::string(storage::kMusicTmpDir) + "/" + id + "." + f.suffix + ".part"
                                          : library::path_for(id, f.suffix);
                uint64_t sz = storage::file_size(p);
                if (unlink(p.c_str()) == 0 && !f.partial) storage::music_add(-static_cast<int64_t>(sz));
            }
    library::replace(std::move(table), std::move(lists));
    library::save();

    std::vector<std::string> todo = plan.resume;
    todo.insert(todo.end(), plan.download.begin(), plan.download.end());
    n_total.store(static_cast<uint16_t>(plan.keep.size() + todo.size()));
    n_done.store(static_cast<uint16_t>(plan.keep.size()));
    bool complete = true;
    for (auto& id : todo) {
        if (should_pause()) {
            complete = false;
            st.store(static_cast<uint8_t>(swp::SyncState::Paused));
            break;
        }
        const ccore::RemoteTrack* rt = nullptr;
        for (auto& r : remote)
            if (r.id == id) rt = &r;
        if (!rt) continue;
        std::string part = std::string(storage::kMusicTmpDir) + "/" + id + "." + rt->suffix + ".part";
        if (!storage::music_reserve(rt->budget)) {
            ESP_LOGW(TAG, "music quota or card space exhausted; stopping downloads");
            complete = false;
            break;
        }
        uint64_t written = 0;
        char q[96];
        snprintf(q, sizeof q, "id=%s&format=%s&maxBitRate=%u", ccore::url_encode(id).c_str(), kFormat, kKbps);
        int64_t t0 = esp_timer_get_time();
        auto r = http::download(url("stream", q), {}, part, 0, 0, should_pause, &written, true);
        if (!r.ok() || written == 0) {
            ESP_LOGW(TAG, "download %s interrupted (%s, %llu bytes); will retry", id.c_str(), r.error.c_str(), written);
            unlink(part.c_str());
            complete = false;
            if (should_pause()) break;
            continue;
        }
        ESP_LOGI(TAG, "track %s: %llu KB in %lld ms", id.c_str(), written / 1024, (esp_timer_get_time() - t0) / 1000);
        std::string final_path = library::path_for(id, rt->suffix);
        unlink(final_path.c_str());
        if (rename(part.c_str(), final_path.c_str()) != 0) {
            complete = false;
            continue;
        }
        storage::music_add(static_cast<int64_t>(written));
        // Drop other copies of this track (an original from before transcoding, stale .parts).
        for (auto& f : local) {
            if (f.id != id || (!f.partial && f.suffix == rt->suffix)) continue;
            std::string p = f.partial ? std::string(storage::kMusicTmpDir) + "/" + id + "." + f.suffix + ".part" : library::path_for(id, f.suffix);
            if (unlink(p.c_str()) == 0 && !f.partial) storage::music_add(-static_cast<int64_t>(f.size));
        }
        library::set_available(id, written);
        // Persist now and then so a reboot mid-sync still lists what is on the card.
        if (n_done.fetch_add(1) % 10 == 9) library::save();
    }
    library::save();
    if (st.load() != static_cast<uint8_t>(swp::SyncState::Paused))
        st.store(static_cast<uint8_t>(complete ? swp::SyncState::Done : swp::SyncState::Failed));
    return complete;
}

void task(void*) {
    // The storage task mounts the card asynchronously (and may wait for a card insert).
    while (!storage::mounted()) vTaskDelay(pdMS_TO_TICKS(2000));
    library::load();
    for (;;) {
        // Wi-Fi only: TODO(LTE) never sync music over cellular data.
        if (net::online() && net::provider() == swp::NetProvider::Wifi && !player::active()) {
            bool ok = sync_once();
            TickType_t wait = ok ? kPeriod : pdMS_TO_TICKS(st.load() == static_cast<uint8_t>(swp::SyncState::Paused) ? 30000 : 10 * 60000);
            xEventGroupWaitBits(ev, kTrigger, pdTRUE, pdFALSE, wait);
        } else {
            xEventGroupWaitBits(ev, kTrigger, pdTRUE, pdFALSE, pdMS_TO_TICKS(30000));
        }
    }
}

void on_net(bool online) {
    if (online) trigger();
}

void on_config(const std::string& key) {
    if (key.rfind("nd_", 0) == 0) trigger();
}

}  // namespace

void start() {
    ev = xEventGroupCreate();
    net::add_listener(on_net);
    config::add_listener(on_config);
    // Lowest application priority: never competes with audio decoding or BLE.
    xTaskCreatePinnedToCore(task, "nd_sync", 6656, nullptr, 2, nullptr, 0);  // ~3 KB used, ~4.5 KB with TLS
}

void trigger() { xEventGroupSetBits(ev, kTrigger); }
swp::SyncState state() { return static_cast<swp::SyncState>(st.load()); }
uint16_t done() { return n_done.load(); }
uint16_t total() { return n_total.load(); }

}  // namespace music::navidrome
