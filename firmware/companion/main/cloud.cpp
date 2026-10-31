#include "cloud.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>

#include "ble_server.hpp"
#include "cJSON.h"
#include "ccore/backoff.hpp"
#include "ccore/subsonic.hpp"
#include "config.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "http.hpp"
#include "music/player.hpp"
#include "net.hpp"
#include "ntfy.hpp"
#include "position.hpp"
#include "storage.hpp"
#include "swp/transfer.hpp"

namespace cloud {
namespace {

constexpr const char* TAG = "cloud";
constexpr int64_t kWeatherPeriodUs = 15LL * 60 * 1000000;
constexpr int64_t kNotifResendUs = 30LL * 1000000;
constexpr size_t kMaxTileBytes = 200 * 1024;

enum class JobKind : uint8_t { Tile, Search, Route, Weather, Wake, MapSync, MapData };
struct Job {
    JobKind kind;
    uint16_t conn;
    swp::MapTileRequest tile;
    swp::RouteRequest route;
    swp::MapSyncRequest sync;
    swp::MapDataCommand data;
    char query[64];
    int32_t lat_e7, lon_e7;
    uint8_t limit;
};

QueueHandle_t jobs;
TaskHandle_t worker = nullptr;
std::mutex mu;

bool have_weather = false;
swp::Weather cached_weather;
int64_t weather_fetched_us = 0;

struct PendingNotif {
    swp::Notification n;
    int64_t sent_us = 0;  // 0 = not yet sent to the watch
};
std::map<uint64_t, PendingNotif> pending;  // keyed by notification id (from the ntfy message id)
std::atomic<bool> srv_ok{false};

// Tile transfer to the watch (one at a time).
struct ActiveTransfer {
    bool active = false;
    uint16_t conn = 0xFFFF;
    std::vector<uint8_t> data;
    swp::TransferSender tx;
    int64_t last_progress_us = 0;
} xfer;
uint16_t next_xfer_id = 1;
std::deque<std::pair<uint16_t, swp::MapTileRequest>> tile_queue;

http::Headers auth_headers() {
    http::Headers h;
    std::string tok = config::get(config::kServerToken);
    if (!tok.empty()) h.emplace_back("Authorization", "Bearer " + tok);
    return h;
}

std::string server() {
    std::string s = config::get(config::kServerUrl);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

double num(const cJSON* o, const char* k, double def = 0) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}

std::string str(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

int16_t c10(double v) { return static_cast<int16_t>(std::lround(std::clamp(v, -300.0, 300.0) * 10)); }

void send_error(uint16_t conn, swp::ErrorCode code, swp::MsgType ref, const std::string& msg) {
    swp::Error e;
    e.code = static_cast<uint8_t>(code);
    e.ref_type = static_cast<uint8_t>(ref);
    e.message = msg;
    ble_server::send(conn, e);
}

bool configured() { return !server().empty(); }

// ---- Weather ----------------------------------------------------------------------------

bool fetch_weather() {
    swp::Position pos;
    if (!position::current(pos) || !net::online() || !configured()) return false;
    char url[256];
    snprintf(url, sizeof url, "%s/v1/weather?lat=%.5f&lon=%.5f", server().c_str(), pos.lat_e7 / 1e7, pos.lon_e7 / 1e7);
    std::vector<uint8_t> body;
    auto r = http::get(url, auth_headers(), body, 16 * 1024);
    srv_ok.store(r.ok());
    net::set_server_ok(r.ok());
    if (!r.ok()) return false;
    body.push_back(0);
    cJSON* j = cJSON_Parse(reinterpret_cast<char*>(body.data()));
    if (!j) return false;
    swp::Weather w;
    w.updated_at = static_cast<uint32_t>(num(j, "updated_at"));
    w.wmo_code = static_cast<uint8_t>(num(j, "code"));
    w.description = str(j, "description");
    w.temp_c10 = c10(num(j, "temp_c"));
    w.low_c10 = c10(num(j, "low_c"));
    w.high_c10 = c10(num(j, "high_c"));
    w.wind_kmh10 = static_cast<uint16_t>(std::lround(std::clamp(num(j, "wind_kmh"), 0.0, 6000.0) * 10));
    w.uv10 = static_cast<uint16_t>(std::lround(std::clamp(num(j, "uv"), 0.0, 6000.0) * 10));
    const cJSON* p = cJSON_GetObjectItemCaseSensitive(j, "precip");
    if (cJSON_IsObject(p)) {
        std::string kind = str(p, "kind");
        w.precip_kind = static_cast<uint8_t>(kind == "rain" ? swp::PrecipKind::Rain : kind == "snow" ? swp::PrecipKind::Snow
                                                              : kind == "mixed" ? swp::PrecipKind::Mixed : kind == "none" ? swp::PrecipKind::None
                                                                                                                  : swp::PrecipKind::Unknown);
        const cJSON* m = cJSON_GetObjectItemCaseSensitive(p, "in_minutes");
        w.precip_in_min = cJSON_IsNumber(m) ? static_cast<int32_t>(m->valuedouble) : -1;
        w.precip_horizon_h = static_cast<uint8_t>(num(p, "horizon_h"));
    }
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "days")) {
        if (w.days.size() >= 7) break;
        swp::WeatherDay d;
        d.date = static_cast<uint32_t>(num(it, "date"));
        d.wmo_code = static_cast<uint8_t>(num(it, "code"));
        d.low_c10 = c10(num(it, "low_c"));
        d.high_c10 = c10(num(it, "high_c"));
        d.precip_prob = static_cast<uint8_t>(std::clamp(num(it, "precip_prob"), 0.0, 100.0));
        d.precip_mm10 = static_cast<uint16_t>(std::clamp(num(it, "precip_mm") * 10, 0.0, 65535.0));
        d.uv_max10 = static_cast<uint16_t>(std::clamp(num(it, "uv_max") * 10, 0.0, 65535.0));
        w.days.push_back(d);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(j, "hours")) {
        if (w.hours.size() >= 24) break;
        swp::WeatherHour h;
        h.time = static_cast<uint32_t>(num(it, "time"));
        h.wmo_code = static_cast<uint8_t>(num(it, "code"));
        h.temp_c10 = c10(num(it, "temp_c"));
        h.precip_prob = static_cast<uint8_t>(std::clamp(num(it, "precip_prob"), 0.0, 100.0));
        w.hours.push_back(h);
    }
    cJSON_Delete(j);
    {
        std::lock_guard<std::mutex> g(mu);
        cached_weather = w;
        have_weather = true;
        weather_fetched_us = esp_timer_get_time();
    }
    ble_server::to_watch(w);
    return true;
}

// ---- Notifications (at-least-once; source: ntfy.cpp) -------------------------------------

void deliver_pending() {
    if (!ble_server::watch_connected()) return;
    int64_t now = esp_timer_get_time();
    std::vector<swp::Notification> out;
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& [id, p] : pending)
            if (p.sent_us == 0 || now - p.sent_us > kNotifResendUs) {
                p.sent_us = now;
                out.push_back(p.n);
                if (out.size() >= 5) break;  // trickle: keep BLE free for interactive traffic
            }
    }
    for (auto& n : out) ble_server::to_watch(n);
}

// ---- Maps -----------------------------------------------------------------------------

std::string tile_meta_err(const swp::MapTileRequest& r) {
    return std::to_string(r.z) + "/" + std::to_string(r.x) + "/" + std::to_string(r.y);
}

// SWT tiles (see server/src/tiles.rs) are stored and forwarded as-is; the watch decodes them.
bool is_swt(const std::vector<uint8_t>& b) { return b.size() > 58 && memcmp(b.data(), "SWT1", 4) == 0; }

bool load_tile(const swp::MapTileRequest& r, std::vector<uint8_t>& tile) {
    if (r.z > 19 || r.x >= (1u << r.z) || r.y >= (1u << r.z)) return false;
    if (storage::tile_read(r.z, r.x, r.y, tile) && is_swt(tile)) return true;  // offline cache first
    if (!net::online() || !configured()) return false;
    char url[200];
    snprintf(url, sizeof url, "%s/v1/maps/tile/%u/%lu/%lu.swt", server().c_str(), r.z, (unsigned long)r.x, (unsigned long)r.y);
    auto res = http::get(url, auth_headers(), tile, kMaxTileBytes);
    if (!res.ok() || !is_swt(tile)) return false;
    storage::tile_write(r.z, r.x, r.y, tile);
    return true;
}

// ---- Offline map sync and helper map data ---------------------------------------------

void sync_status(uint16_t conn, swp::MapSyncState st, uint32_t done, uint32_t kbytes, const std::string& msg) {
    swp::MapSyncStatus s;
    s.state = static_cast<uint8_t>(st);
    s.done = done;
    s.kbytes = kbytes;
    s.message = msg;
    ble_server::send(conn, s);
}

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// Downloads one tile bundle (all tiles around a point, a single HTTPS response) to the SD
// card, then splits it into the tile cache.
void do_map_sync(const Job& j) {
    using St = swp::MapSyncState;
    if (!net::online() || !configured()) return sync_status(j.conn, St::Failed, 0, 0, "Companion is offline");
    if (!storage::mounted()) return sync_status(j.conn, St::Failed, 0, 0, "No SD card");
    sync_status(j.conn, St::Running, 0, 0, "Downloading");
    char url[256];
    snprintf(url, sizeof url, "%s/v1/maps/bundle?lat=%.6f&lon=%.6f&radius_m=%u&zmin=%u&zmax=%u", server().c_str(), j.sync.lat_e7 / 1e7,
             j.sync.lon_e7 / 1e7, j.sync.radius_m, j.sync.zmin, j.sync.zmax);
    std::string path = std::string(storage::kMapsDir) + "/.bundle";
    uint64_t got = 0;
    auto r = http::download(url, auth_headers(), path, 0, 0, [] { return !net::online(); }, &got);
    if (r.status == 503) return sync_status(j.conn, St::Preparing, 0, 0, "Helper is building map data for this area; try again in a few minutes");
    if (!r.ok()) return sync_status(j.conn, St::Failed, 0, 0, r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return sync_status(j.conn, St::Failed, 0, 0, "Bundle missing");
    uint8_t hdr[13];
    uint32_t done = 0;
    uint64_t bytes = 0;
    bool ok = fread(hdr, 1, 4, f) == 4 && memcmp(hdr, "SWB1", 4) == 0;
    std::vector<uint8_t> tile;
    while (ok) {
        if (fread(hdr, 1, 1, f) != 1) { ok = false; break; }
        if (hdr[0] == 0xFF) break;  // end marker
        if (fread(hdr + 1, 1, 12, f) != 12) { ok = false; break; }
        uint8_t z = hdr[0];
        uint32_t x = rd32(hdr + 1), y = rd32(hdr + 5), len = rd32(hdr + 9);
        if (z > 19 || len > kMaxTileBytes) { ok = false; break; }
        tile.resize(len);
        if (fread(tile.data(), 1, len, f) != len) { ok = false; break; }
        if (is_swt(tile) && storage::tile_write(z, x, y, tile)) {
            ++done;
            bytes += len;
        }
        if (done % 100 == 0) sync_status(j.conn, St::Running, done, static_cast<uint32_t>(bytes / 1024), "Saving to SD card");
    }
    fclose(f);
    unlink(path.c_str());
    storage::flush_index();
    sync_status(j.conn, ok ? St::Done : St::Failed, done, static_cast<uint32_t>(bytes / 1024), ok ? "Offline maps ready" : "Bundle was cut short");
}

void do_map_data(const Job& j) {
    swp::MapDataStatus out;
    if (!net::online() || !configured()) {
        out.status = static_cast<uint8_t>(swp::ErrorCode::NoNetwork);
        out.error = "Companion is offline";
        ble_server::send(j.conn, out);
        return;
    }
    std::vector<uint8_t> body;
    http::Result r;
    auto action = static_cast<swp::MapDataAction>(j.data.action);
    if (action == swp::MapDataAction::Update) {
        char json[96];
        snprintf(json, sizeof json, "{\"lat\":%.6f,\"lon\":%.6f}", j.data.lat_e7 / 1e7, j.data.lon_e7 / 1e7);
        r = http::post_json(server() + "/v1/maps/data/update", auth_headers(), json, body, 4096);
    } else if (action == swp::MapDataAction::Delete) {
        r = http::post_json(server() + "/v1/maps/data/delete", auth_headers(), "{}", body, 4096);
    } else {
        r = http::get(server() + "/v1/maps/data", auth_headers(), body, 4096);
    }
    body.push_back(0);
    cJSON* root = cJSON_Parse(reinterpret_cast<char*>(body.data()));
    if (!r.ok()) {
        out.status = static_cast<uint8_t>(swp::ErrorCode::Unavailable);
        out.error = root ? str(root, "error") : "";
        if (out.error.empty()) out.error = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
    } else if (root) {
        out.active = str(root, "active");
        out.building = str(root, "building");
        out.error = str(root, "last_error");
    }
    if (root) cJSON_Delete(root);
    ble_server::send(j.conn, out);
}

void start_next_tile() {
    // tile_queue is worker-only; xfer is shared with on_transfer_ack (BTC task) under `mu`.
    for (;;) {
        {
            std::lock_guard<std::mutex> g(mu);
            if (xfer.active || tile_queue.empty()) return;
        }
        auto [conn, req] = tile_queue.front();
        tile_queue.pop_front();
        std::vector<uint8_t> png;
        if (!load_tile(req, png)) {
            send_error(conn, swp::ErrorCode::Unavailable, swp::MsgType::MapTileRequest, tile_meta_err(req));
            continue;
        }
        std::lock_guard<std::mutex> g(mu);
        xfer.active = true;
        xfer.conn = conn;
        xfer.data = std::move(png);
        std::vector<uint8_t> meta;
        swp::Writer w(meta);
        w.u8(req.z);
        w.u32(req.x);
        w.u32(req.y);
        uint16_t mtu = ble_server::mtu(conn);
        // One chunk per notification: MTU - ATT(3) - frame(10) - chunk header(8).
        uint16_t chunk = static_cast<uint16_t>(std::clamp<int>(mtu - 21, 20, 480));
        xfer.tx.start(next_xfer_id++, swp::TransferKind::MapTile, xfer.data.data(), static_cast<uint32_t>(xfer.data.size()), chunk,
                      req.resume_offset, meta);
        xfer.last_progress_us = esp_timer_get_time();
        ble_server::send(conn, xfer.tx.begin_message());
    }
}

void pump_transfer() {
    std::lock_guard<std::mutex> g(mu);
    if (!xfer.active) return;
    if (xfer.tx.done() || esp_timer_get_time() - xfer.last_progress_us > 15LL * 1000000) {
        if (!xfer.tx.done()) ESP_LOGW(TAG, "tile transfer timed out");
        xfer.active = false;
        xfer.data.clear();
        return;
    }
    // Prioritise audio: while A2DP streams keep fewer frames in flight.
    int max_q = music::player::streaming() ? 2 : 6;
    while (ble_server::queued(xfer.conn) < max_q) {
        auto c = xfer.tx.next_chunk();
        if (!c) break;
        if (!ble_server::send(xfer.conn, *c)) break;
    }
}

void do_search(const Job& j) {
    if (!net::online() || !configured()) return send_error(j.conn, swp::ErrorCode::NoNetwork, swp::MsgType::MapSearchRequest, "offline");
    char url[320];
    std::string q = ccore::url_encode(j.query);
    snprintf(url, sizeof url, "%s/v1/maps/search?q=%s&lat=%.6f&lon=%.6f&limit=%u", server().c_str(), q.c_str(), j.lat_e7 / 1e7, j.lon_e7 / 1e7,
             j.limit);
    std::vector<uint8_t> body;
    auto r = http::get(url, auth_headers(), body, 32 * 1024);
    if (!r.ok()) return send_error(j.conn, swp::ErrorCode::Unavailable, swp::MsgType::MapSearchRequest, "search failed");
    body.push_back(0);
    cJSON* root = cJSON_Parse(reinterpret_cast<char*>(body.data()));
    swp::MapSearchResults res;
    res.query = j.query;
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "places")) {
        if (res.places.size() >= 12) break;
        swp::Place p;
        p.name = str(it, "name");
        p.category = str(it, "category");
        p.address = str(it, "address");
        p.lat_e7 = static_cast<int32_t>(std::lround(num(it, "lat") * 1e7));
        p.lon_e7 = static_cast<int32_t>(std::lround(num(it, "lon") * 1e7));
        p.distance_m = static_cast<uint32_t>(num(it, "distance_m"));
        res.places.push_back(std::move(p));
    }
    cJSON_Delete(root);
    ble_server::send(j.conn, res);
}

void do_route(const Job& j) {
    if (!net::online() || !configured()) return send_error(j.conn, swp::ErrorCode::NoNetwork, swp::MsgType::RouteRequest, "offline");
    static const char* modes[] = {"walk", "bike", "drive"};
    char url[320];
    snprintf(url, sizeof url, "%s/v1/maps/route?from=%.6f,%.6f&to=%.6f,%.6f&mode=%s", server().c_str(), j.route.from_lat_e7 / 1e7,
             j.route.from_lon_e7 / 1e7, j.route.to_lat_e7 / 1e7, j.route.to_lon_e7 / 1e7, modes[j.route.mode % 3]);
    std::vector<uint8_t> body;
    auto r = http::get(url, auth_headers(), body, 64 * 1024);
    swp::Route rt;
    if (!r.ok()) {
        rt.status = static_cast<uint8_t>(swp::ErrorCode::Unavailable);
        ble_server::send(j.conn, rt);
        return;
    }
    body.push_back(0);
    cJSON* root = cJSON_Parse(reinterpret_cast<char*>(body.data()));
    rt.status = str(root, "status") == "ok" ? 0 : static_cast<uint8_t>(swp::ErrorCode::NotFound);
    rt.distance_m = static_cast<uint32_t>(num(root, "distance_m"));
    rt.duration_s = static_cast<uint32_t>(num(root, "duration_s"));
    const cJSON* it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "points")) {
        if (rt.points.size() >= 200 || !cJSON_IsArray(it) || cJSON_GetArraySize(it) < 2) continue;
        swp::RoutePoint p;
        p.lat_e7 = static_cast<int32_t>(std::lround(cJSON_GetArrayItem(it, 0)->valuedouble * 1e7));
        p.lon_e7 = static_cast<int32_t>(std::lround(cJSON_GetArrayItem(it, 1)->valuedouble * 1e7));
        rt.points.push_back(p);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "steps")) {
        if (rt.steps.size() >= 24) break;
        swp::RouteStep s;
        s.instruction = str(it, "instruction");
        s.distance_m = static_cast<uint32_t>(num(it, "distance_m"));
        s.point_index = static_cast<uint16_t>(num(it, "point_index"));
        rt.steps.push_back(std::move(s));
    }
    cJSON_Delete(root);
    // Keep the frame within the SWP payload limit by dropping steps first, then points.
    while (swp::encode_payload(rt).size() > swp::kMaxPayload - 64 && !rt.steps.empty()) rt.steps.pop_back();
    while (swp::encode_payload(rt).size() > swp::kMaxPayload - 64 && rt.points.size() > 2) rt.points.erase(rt.points.begin() + 1);
    ble_server::send(j.conn, rt);
}

void worker_task(void*) {
    Job j;
    for (;;) {
        bool busy;
        {
            std::lock_guard<std::mutex> g(mu);
            busy = xfer.active;
        }
        TickType_t wait = pdMS_TO_TICKS(busy ? 20 : 1000);
        if (xQueueReceive(jobs, &j, wait) == pdTRUE) {
            switch (j.kind) {
                case JobKind::Tile:
                    if (tile_queue.size() < 12) tile_queue.emplace_back(j.conn, j.tile);
                    break;
                case JobKind::Search: do_search(j); break;
                case JobKind::Route: do_route(j); break;
                case JobKind::Weather: {
                    bool fresh;
                    {
                        std::lock_guard<std::mutex> g(mu);
                        fresh = have_weather && esp_timer_get_time() - weather_fetched_us < kWeatherPeriodUs;
                    }
                    if (fresh) {
                        swp::Weather w;
                        if (weather(w)) ble_server::send(j.conn, w);
                    } else if (!fetch_weather()) {
                        swp::Weather w;
                        if (weather(w)) ble_server::send(j.conn, w);  // stale is better than nothing
                    }
                    break;
                }
                case JobKind::MapSync: do_map_sync(j); break;
                case JobKind::MapData: do_map_data(j); break;
                case JobKind::Wake: break;
            }
        }
        start_next_tile();
        pump_transfer();

        int64_t now = esp_timer_get_time();
        if (net::online() && configured()) {
            bool stale;
            {
                std::lock_guard<std::mutex> g(mu);
                stale = !have_weather || now - weather_fetched_us > kWeatherPeriodUs;
            }
            static int64_t last_wx_try = 0;
            if (stale && now - last_wx_try > 60LL * 1000000) {
                last_wx_try = now;
                fetch_weather();
            }
        }
        deliver_pending();
    }
}

void post(const Job& j) { xQueueSend(jobs, &j, 0); }

void on_net(bool online) {
    if (online) {
        Job j = {};
        j.kind = JobKind::Wake;
        post(j);
    }
}

}  // namespace

void start() {
    jobs = xQueueCreate(16, sizeof(Job));
    net::add_listener(on_net);
    xTaskCreatePinnedToCore(worker_task, "cloud", 8192, nullptr, 4, &worker, 0);
}

void request_tile(uint16_t conn, const swp::MapTileRequest& r) {
    Job j = {};
    j.kind = JobKind::Tile;
    j.conn = conn;
    j.tile = r;
    post(j);
}

void on_transfer_ack(uint16_t conn, const swp::TransferAck& a) {
    // Called from the BTC task; TransferSender state is only otherwise touched by the
    // worker, so hand over through the queue would add latency. A short lock is enough.
    std::lock_guard<std::mutex> g(mu);
    if (xfer.active && xfer.conn == conn && xfer.tx.on_ack(a)) {
        xfer.last_progress_us = esp_timer_get_time();
        if (xfer.tx.done()) xfer.active = false;
    }
    Job j = {};
    j.kind = JobKind::Wake;
    post(j);
}

void request_search(uint16_t conn, const swp::MapSearchRequest& r) {
    Job j = {};
    j.kind = JobKind::Search;
    j.conn = conn;
    strlcpy(j.query, r.query.c_str(), sizeof j.query);
    j.lat_e7 = r.lat_e7;
    j.lon_e7 = r.lon_e7;
    j.limit = r.limit ? std::min<uint8_t>(r.limit, 12) : 10;
    post(j);
}

void request_map_sync(uint16_t conn, const swp::MapSyncRequest& r) {
    Job j = {};
    j.kind = JobKind::MapSync;
    j.conn = conn;
    j.sync = r;
    j.sync.radius_m = std::clamp<uint16_t>(r.radius_m, 200, 10000);
    j.sync.zmax = std::min<uint8_t>(r.zmax, 18);
    j.sync.zmin = std::min(r.zmin, j.sync.zmax);
    post(j);
}

void request_map_data(uint16_t conn, const swp::MapDataCommand& c) {
    Job j = {};
    j.kind = JobKind::MapData;
    j.conn = conn;
    j.data = c;
    post(j);
}

void request_route(uint16_t conn, const swp::RouteRequest& r) {
    Job j = {};
    j.kind = JobKind::Route;
    j.conn = conn;
    j.route = r;
    post(j);
}

void request_weather(uint16_t conn) {
    Job j = {};
    j.kind = JobKind::Weather;
    j.conn = conn;
    post(j);
}

void ingest_notification(const swp::Notification& n) {
    {
        std::lock_guard<std::mutex> g(mu);
        if (pending.count(n.id) || pending.size() >= 64) return;
        PendingNotif p;
        p.n = n;
        pending[n.id] = p;
    }
    Job j = {};
    j.kind = JobKind::Wake;  // deliver from the worker task
    post(j);
}

void on_notification_ack(uint64_t id) {
    {
        std::lock_guard<std::mutex> g(mu);
        pending.erase(id);
    }
    ntfy::on_ack(id);
}

void on_watch_connected() {
    std::lock_guard<std::mutex> g(mu);
    for (auto& [id, p] : pending) p.sent_us = 0;  // redeliver everything not acknowledged
}

void on_watch_disconnected() {
    std::lock_guard<std::mutex> g(mu);
    xfer.active = false;  // queued tile requests for a gone connection fail fast in send()
}

bool weather(swp::Weather& out) {
    std::lock_guard<std::mutex> g(mu);
    if (!have_weather) return false;
    out = cached_weather;
    return true;
}

uint16_t pending_notifications() {
    std::lock_guard<std::mutex> g(mu);
    return static_cast<uint16_t>(pending.size());
}

bool server_ok() { return srv_ok.load(); }

}  // namespace cloud
