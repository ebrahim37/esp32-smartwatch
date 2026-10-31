#include "music/navidrome.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>

#include "cJSON.h"
#include "ccore/quota.hpp"
#include "ccore/subsonic.hpp"
#include "ccore/sync_plan.hpp"
#include "config.hpp"
#include "esp_log.h"
#include "esp_random.h"
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

http::Headers headers() {
    http::Headers h;
    std::string name = config::get(config::kNdHeader), val = config::get(config::kNdHeaderValue);
    if (!name.empty() && !val.empty()) h.emplace_back(name, val);
    return h;
}

std::string url(const char* method, const std::string& extra) {
    return ccore::subsonic_url(config::get(config::kNdUrl), method, auth(), salt(), extra);
}

cJSON* api(const char* method, const std::string& extra, std::vector<uint8_t>& body) {
    auto r = http::get(url(method, extra), headers(), body, 1024 * 1024);
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

void log_available_playlists() {
    std::vector<uint8_t> body;
    cJSON* root = api("getPlaylists", "", body);
    if (!root) return;
    const cJSON* pls = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "subsonic-response"), "playlists"), "playlist");
    const cJSON* p;
    cJSON_ArrayForEach(p, pls) ESP_LOGI(TAG, "available playlist: id=%s name=\"%s\"", jstr(p, "id").c_str(), jstr(p, "name").c_str());
    cJSON_Delete(root);
}

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

bool sync_once() {
    std::string pl_id = config::get(config::kPrototypePlaylistIdKey);
    if (config::get(config::kNdUrl).empty() || config::get(config::kNdUser).empty()) {
        st.store(static_cast<uint8_t>(swp::SyncState::NotConfigured));
        return false;
    }
    if (pl_id.empty()) {
        ESP_LOGW(TAG, "no playlist configured (set nd_playlist / PROTOTYPE_PLAYLIST_ID)");
        log_available_playlists();
        st.store(static_cast<uint8_t>(swp::SyncState::NotConfigured));
        return false;
    }
    if (!storage::mounted()) return false;
    st.store(static_cast<uint8_t>(swp::SyncState::Running));

    std::vector<uint8_t> body;
    cJSON* root = api("getPlaylist", "id=" + ccore::url_encode(pl_id), body);
    if (!root) {
        st.store(static_cast<uint8_t>(swp::SyncState::Failed));
        return false;  // never delete anything on a failed metadata fetch
    }
    const cJSON* pl = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(root, "subsonic-response"), "playlist");
    library::Playlist lib;
    lib.id = pl_id;
    lib.name = jstr(pl, "name");
    std::vector<ccore::RemoteTrack> remote;
    const cJSON* e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(pl, "entry")) {
        library::Track t;
        t.id = jstr(e, "id");
        t.title = jstr(e, "title");
        t.artist = jstr(e, "artist");
        t.album = jstr(e, "album");
        t.suffix = jstr(e, "suffix");
        const cJSON* d = cJSON_GetObjectItemCaseSensitive(e, "duration");
        t.duration_s = cJSON_IsNumber(d) ? static_cast<uint32_t>(d->valuedouble) : 0;
        const cJSON* sz = cJSON_GetObjectItemCaseSensitive(e, "size");
        t.size = cJSON_IsNumber(sz) ? static_cast<uint64_t>(sz->valuedouble) : 0;
        for (auto& c : t.suffix) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        if (!ccore::safe_id(t.id) || !ccore::supported_suffix(t.suffix)) {
            ESP_LOGW(TAG, "skipping unsupported track %s (%s)", t.id.c_str(), t.suffix.c_str());
            continue;
        }
        remote.push_back({t.id, t.suffix, t.size});
        lib.tracks.push_back(std::move(t));
    }
    cJSON_Delete(root);

    auto local = scan_local();
    auto plan = ccore::plan_sync(remote, local, ccore::kMusicQuota);
    ESP_LOGI(TAG, "playlist \"%s\": %u tracks, keep %u, download %u, resume %u, remove %u, over quota %u", lib.name.c_str(),
             (unsigned)remote.size(), (unsigned)plan.keep.size(), (unsigned)plan.download.size(), (unsigned)plan.resume.size(),
             (unsigned)plan.remove.size(), (unsigned)plan.over_quota.size());

    // Remove files no longer in the playlist (the playlist fetch succeeded, so this is safe).
    for (auto& id : plan.remove)
        for (auto& f : local)
            if (f.id == id) {
                std::string p = f.partial ? std::string(storage::kMusicTmpDir) + "/" + id + "." + f.suffix + ".part"
                                          : library::path_for(id, f.suffix);
                uint64_t sz = storage::file_size(p);
                if (unlink(p.c_str()) == 0 && !f.partial) storage::music_add(-static_cast<int64_t>(sz));
            }
    library::replace(lib);
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
        uint64_t have = storage::file_size(part);
        if (have > rt->size) {
            unlink(part.c_str());
            have = 0;
        }
        if (!storage::music_reserve(rt->size - have)) {
            ESP_LOGW(TAG, "music quota or card space exhausted; stopping downloads");
            complete = false;
            break;
        }
        uint64_t written = 0;
        auto r = http::download(url("download", "id=" + ccore::url_encode(id)), headers(), part, have, rt->size, should_pause, &written);
        if (!r.ok() || written != rt->size) {
            ESP_LOGW(TAG, "download %s interrupted (%s, %llu/%llu bytes); will resume", id.c_str(), r.error.c_str(), written, rt->size);
            complete = false;
            if (r.error == "size mismatch") unlink(part.c_str());
            if (should_pause()) break;
            continue;
        }
        std::string final_path = library::path_for(id, rt->suffix);
        unlink(final_path.c_str());
        if (rename(part.c_str(), final_path.c_str()) != 0) {
            complete = false;
            continue;
        }
        storage::music_add(static_cast<int64_t>(rt->size));
        n_done.fetch_add(1);
        library::replace(lib);  // refresh availability as tracks land
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
    xTaskCreatePinnedToCore(task, "nd_sync", 8192, nullptr, 2, nullptr, 0);
}

void trigger() { xEventGroupSetBits(ev, kTrigger); }
swp::SyncState state() { return static_cast<swp::SyncState>(st.load()); }
uint16_t done() { return n_done.load(); }
uint16_t total() { return n_total.load(); }

}  // namespace music::navidrome
