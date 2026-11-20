// Navidrome sync over any network, including Tailscale on cellular (port of firmware/companion/main/music/navidrome.cpp). Runs on its own
// background queue with blocking requests; downloads continue during playback because the player
// works on a snapshot of its playlist.
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string_view>
#include <unordered_map>

#include "app.hpp"
#include "ccore/quota.hpp"
#include "ccore/subsonic.hpp"
#include "ccore/sync_plan.hpp"

namespace navidrome {
namespace {

constexpr const char* kFormat = "mp3";
constexpr unsigned kKbps = 128;
constexpr int kWorkers = 3;
constexpr double kPeriodS = 6 * 3600, kRetryS = 600, kPausedRetryS = 30;

dispatch_queue_t queue;
std::atomic<uint8_t> st{static_cast<uint8_t>(swp::SyncState::Idle)};
std::atomic<uint16_t> n_done{0}, n_total{0};
std::atomic<uint64_t> music_used{0};
bool running = false, again = false;
std::mutex err_mu;
std::string last_err;  // guarded by err_mu

void set_error(const std::string& e) {
    std::lock_guard<std::mutex> g(err_mu);
    last_err = e;
}
double next_due = 0;

std::string salt() {
    char b[17];
    snprintf(b, sizeof b, "%08x%08x", arc4random(), arc4random());
    return b;
}

std::string url(const char* method, const std::string& extra) {
    ccore::SubsonicAuth a;
    a.user = cfg::get(cfg::kNdUser);
    a.password = cfg::get(cfg::kNdPass);
    a.client = "sw-ios";
    return ccore::subsonic_url(cfg::get(cfg::kNdUrl), method, a, salt(), extra);
}

// The `auth` header, like ntfy's, for a reverse proxy in front of Navidrome.
http::Options options() {
    http::Options o;
    std::string token = cfg::get(cfg::kNdToken);
    if (!token.empty()) o.headers.emplace_back("auth", token);
    return o;
}

id api(const char* method, const std::string& extra) {
    http::Options o = options();
    o.max_response = 16 * 1024 * 1024;
    o.timeout_s = 60;
    auto r = http::request_sync(url(method, extra), o);
    if (!r.ok()) {
        NSLog(@"%s failed: %s (HTTP %d)", method, r.error.c_str(), r.status);
        set_error(std::string(method) + ": " + (r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error));
        return nil;
    }
    id resp = jval(jparse(r.body), @"subsonic-response");
    if (jstr(resp, @"status") != "ok") {
        std::string msg = jstr(jval(resp, @"error"), @"message");
        NSLog(@"%s failed: %s", method, msg.c_str());
        if (msg.empty()) {
            msg = "unexpected reply: HTTP " + std::to_string(r.status) + ", " + r.mime + ": ";
            for (size_t i = 0; i < r.body.size() && i < 120; ++i) msg += r.body[i] >= 0x20 && r.body[i] < 0x7f ? static_cast<char>(r.body[i]) : ' ';
        }
        set_error(std::string(method) + ": " + msg);
        return nil;
    }
    return resp;
}

bool parse_song(id e, library::Track& t) {
    t.id = jstr(e, @"id");
    if (!ccore::safe_id(t.id)) {
        NSLog(@"skipping track with unsafe id");
        return false;
    }
    t.title = jstr(e, @"title");
    t.artist = jstr(e, @"artist");
    t.album = jstr(e, @"album");
    t.duration_s = static_cast<uint32_t>(std::max(jnum(e, @"duration"), 0.0));
    t.suffix = kFormat;  // downloaded transcoded
    t.size = 0;          // filled in from the disk
    return true;
}

uint64_t budget_for(const library::Track& t) { return (t.duration_s + 5) * kKbps * 125ull; }

std::vector<ccore::LocalFile> scan_local() {
    std::vector<ccore::LocalFile> out;
    uint64_t used = 0;
    if (DIR* d = opendir(paths::music().c_str())) {
        while (struct dirent* e = readdir(d)) {
            std::string n = e->d_name;
            size_t dot = n.find('.');
            if (n.empty() || n[0] == '.' || n == "library.json" || dot == std::string::npos) continue;
            struct stat s;
            if (stat((paths::music() + "/" + n).c_str(), &s) != 0 || !S_ISREG(s.st_mode)) continue;
            ccore::LocalFile f;
            f.id = n.substr(0, dot);
            f.suffix = n.substr(dot + 1);
            f.size = static_cast<uint64_t>(s.st_size);
            if (ccore::safe_id(f.id)) {
                used += f.size;
                out.push_back(std::move(f));
            }
        }
        closedir(d);
    }
    music_used.store(used);
    return out;
}

// Everything on the server, in download priority order. Fails (and the caller deletes nothing)
// if any metadata request fails.
bool fetch_catalogue(std::vector<library::Track>& table, std::vector<library::Playlist>& lists) {
    std::unordered_map<std::string, uint32_t> by_id;
    auto add = [&](library::Track&& t) -> uint32_t {
        auto it = by_id.find(t.id);
        if (it != by_id.end()) return it->second;
        uint32_t i = static_cast<uint32_t>(table.size());
        by_id.emplace(t.id, i);
        table.push_back(std::move(t));
        return i;
    };

    id root = api("getPlaylists", "");
    if (!root) return false;
    for (id p in jarr(jval(root, @"playlists"), @"playlist")) {
        library::Playlist pl;
        pl.id = jstr(p, @"id");
        pl.name = jstr(p, @"name");
        pl.duration_s = static_cast<uint32_t>(std::max(jnum(p, @"duration"), 0.0));
        if (!pl.id.empty() && pl.id != library::kAllSongsId) lists.push_back(std::move(pl));
    }
    for (auto& pl : lists) {
        root = api("getPlaylist", "id=" + ccore::url_encode(pl.id));
        if (!root) return false;
        for (id e in jarr(jval(root, @"playlist"), @"entry")) {
            library::Track t;
            if (parse_song(e, t)) pl.tracks.push_back(add(std::move(t)));
        }
    }
    // The rest of the library: search3 with an empty query lists every song (Navidrome).
    constexpr unsigned kPage = 200;
    for (unsigned offset = 0;; offset += kPage) {
        char q[96];
        snprintf(q, sizeof q, "query=&artistCount=0&albumCount=0&songCount=%u&songOffset=%u", kPage, offset);
        root = api("search3", q);
        if (!root) return false;
        NSArray* songs = jarr(jval(root, @"searchResult3"), @"song");
        size_t before = table.size();
        for (id e in songs) {
            library::Track t;
            if (parse_song(e, t)) add(std::move(t));
        }
        if (songs.count < kPage || table.size() == before) break;  // the second: a server ignoring the offset
    }
    return true;
}

bool should_pause() { return !net::online(); }

// Sync queue.
bool sync_once() {
    if (cfg::get(cfg::kNdUrl).empty() || cfg::get(cfg::kNdUser).empty()) {
        st.store(static_cast<uint8_t>(swp::SyncState::NotConfigured));
        return false;
    }
    st.store(static_cast<uint8_t>(swp::SyncState::Running));
    set_error("");
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
    {
        std::unordered_map<std::string_view, uint64_t> sizes;
        for (auto& f : local)
            if (f.suffix == kFormat) sizes[f.id] = f.size;
        for (auto& t : table)
            if (auto f = sizes.find(t.id); f != sizes.end()) {
                t.size = f->second;
                t.available = t.size > 0;
            }
    }
    auto plan = ccore::plan_sync(remote, local, UINT64_MAX);  // no quota: only free space limits downloads
    NSLog(@"sync: %zu playlists, %zu tracks: keep %zu, download %zu, remove %zu, over quota %zu", lists.size(), remote.size(), plan.keep.size(),
          plan.download.size() + plan.resume.size(), plan.remove.size(), plan.over_quota.size());
    for (auto& id : plan.remove)
        for (auto& f : local)
            if (f.id == id && unlink(library::path_for(id, f.suffix).c_str()) == 0) music_used -= f.size;
    library::replace(std::move(table), std::move(lists));
    library::save();
    {
        swp::Playlists p;
        p.playlists = library::playlists();
        ble::send(p);
    }

    std::vector<std::string> todo = plan.resume;
    todo.insert(todo.end(), plan.download.begin(), plan.download.end());
    n_total.store(static_cast<uint16_t>(std::min<size_t>(plan.keep.size() + todo.size(), UINT16_MAX)));
    n_done.store(static_cast<uint16_t>(std::min<size_t>(plan.keep.size(), UINT16_MAX)));
    std::unordered_map<std::string, uint64_t> budgets;
    for (auto& r : remote) budgets[r.id] = r.budget;

    std::string tmp_dir = paths::music() + "/.tmp";
    std::atomic<size_t> next{0};
    std::atomic<bool> complete{true}, paused{false}, full{false};
    auto worker = [&] {
        for (;;) {
            size_t i = next.fetch_add(1);
            if (i >= todo.size() || paused.load() || full.load()) return;
            if (should_pause()) {
                paused.store(true);
                return;
            }
            const std::string& id = todo[i];
            if (paths::free_bytes() < budgets[id] + ccore::kMinFreeReserve) {
                NSLog(@"free space exhausted; stopping downloads");
                set_error("Phone storage is full");
                full.store(true);
                return;
            }
            char query[96];
            snprintf(query, sizeof query, "id=%s&format=%s&maxBitRate=%u", ccore::url_encode(id).c_str(), kFormat, kKbps);
            std::string part = tmp_dir + "/" + id + "." + kFormat + ".part";
            http::Options o = options();
            o.max_response = 256 * 1024 * 1024;
            o.timeout_s = 120;
            auto r = http::download_sync(url("stream", query), part, o);
            struct stat s;
            bool audio = r.ok() && r.mime.rfind("text/", 0) != 0 && r.mime.find("json") == std::string::npos && r.mime.find("xml") == std::string::npos;  // Subsonic errors arrive as 200
            if (!audio || stat(part.c_str(), &s) != 0 || s.st_size == 0) {
                NSLog(@"download %s failed (%s, %s); will retry", id.c_str(), r.error.c_str(), r.mime.c_str());
                set_error("Download: " + (!r.error.empty() ? r.error : r.ok() ? "server sent " + r.mime : "HTTP " + std::to_string(r.status)));
                unlink(part.c_str());
                complete.store(false);
                continue;
            }
            std::string final_path = library::path_for(id, kFormat);
            if (rename(part.c_str(), final_path.c_str()) != 0) {
                set_error("Could not store a song");
                complete.store(false);
                continue;
            }
            music_used += static_cast<uint64_t>(s.st_size);
            for (auto& f : local)  // an original kept by the ESP32-era format of this id
                if (f.id == id && f.suffix != kFormat && unlink(library::path_for(id, f.suffix).c_str()) == 0) music_used -= f.size;
            library::set_available(id, static_cast<uint64_t>(s.st_size));
            if (n_done.fetch_add(1) % 10 == 9) library::save();  // a crash mid-sync still lists what is on disk
        }
    };
    dispatch_apply(kWorkers, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(size_t) { worker(); });
    library::save();
    rmdir(tmp_dir.c_str());
    if (paused.load()) {
        st.store(static_cast<uint8_t>(swp::SyncState::Paused));
        return false;
    }
    bool ok = complete.load() && !full.load();
    st.store(static_cast<uint8_t>(ok ? swp::SyncState::Done : swp::SyncState::Failed));
    return ok;
}

void check() {
    if (running) return;
    if (!net::online() || mono_s() < next_due) return;
    running = true;
    again = false;
    dispatch_async(queue, ^{
        bool ok = sync_once();
        dispatch_async(dispatch_get_main_queue(), ^{
            running = false;
            auto s = static_cast<swp::SyncState>(st.load());
            next_due = again ? 0 : mono_s() + (ok || s == swp::SyncState::NotConfigured ? kPeriodS : s == swp::SyncState::Paused ? kPausedRetryS : kRetryS);
            dispatch::broadcast_status();
            check();
        });
    });
}

}  // namespace

void start() {
    queue = dispatch_queue_create("sw.navidrome", DISPATCH_QUEUE_SERIAL);
    dispatch_async(queue, ^{
        library::load();
        scan_local();
        swp::Playlists p;
        p.playlists = library::playlists();
        ble::send(p);  // the watch may have asked before the library was loaded
    });
    net::add_listener([] { check(); });
    cfg::add_listener([](const std::string& key) {
        if (key.rfind("nd_", 0) == 0) trigger();
    });
    [NSTimer scheduledTimerWithTimeInterval:30 repeats:YES block:^(NSTimer*) { check(); }];
    dispatch_async(dispatch_get_main_queue(), ^{ check(); });
}

void trigger() {
    next_due = 0;
    again = running;
    check();
}

std::string last_error() {
    std::lock_guard<std::mutex> g(err_mu);
    return last_err;
}

swp::SyncState state() { return static_cast<swp::SyncState>(st.load()); }
uint16_t done() { return n_done.load(); }
uint16_t total() { return n_total.load(); }
uint64_t used_bytes() { return music_used.load(); }

}  // namespace navidrome
