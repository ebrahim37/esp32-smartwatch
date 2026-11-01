#include "cloud.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <new>

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

constexpr size_t kMaxTileQueue = 24;
constexpr size_t kReadyAhead = 2;  // tiles fetched ahead of the BLE transfer

enum class JobKind : uint8_t { Tile, Search, Route, Weather, Wake, MapData, Http };
struct Job {
    JobKind kind;
    uint16_t conn;
    swp::MapTileRequest tile;
    swp::RouteRequest route;
    swp::MapDataCommand data;
    swp::HttpRequest* http;  // owned by the job
    char query[64];
    int32_t lat_e7, lon_e7;
    uint8_t limit;
};

QueueHandle_t jobs;
TaskHandle_t worker = nullptr;
TaskHandle_t xfer_task_handle = nullptr;
std::mutex mu;
http::Session helper;  // keep-alive connection to the helper, used only by the worker task
std::atomic<int64_t> last_tile_request_us{0};

bool have_weather = false;
swp::Weather cached_weather;
int64_t weather_fetched_us = 0;

struct PendingNotif {
    swp::Notification n;
    int64_t sent_us = 0;  // 0 = not yet sent to the watch
};
std::map<uint64_t, PendingNotif> pending;  // keyed by notification id (from the ntfy message id)
std::atomic<bool> srv_ok{false};

// Bulk transfer to the watch (one at a time): map tiles and large HTTP response bodies.
// Chunks are pumped by xfer_task, so the worker can fetch the next tile meanwhile.
struct ActiveTransfer {
    bool active = false;
    uint16_t conn = 0xFFFF;
    std::vector<uint8_t> data;
    swp::TransferSender tx;
    int64_t last_progress_us = 0;
    int64_t started_us = 0;
    char label[40] = "";  // for the timing log
} xfer;
uint16_t next_xfer_id = 1;
// Worker-only tile pipeline: requested -> fetched (ready) -> transferred; tiles fetched from
// the helper are written to the SD cache afterwards, off the critical path.
std::deque<std::pair<uint16_t, swp::MapTileRequest>> tile_queue;
struct ReadyTile {
    uint16_t conn;
    swp::MapTileRequest req;
    std::vector<uint8_t> data;
    char label[40];
};
std::deque<ReadyTile> ready;
struct SdTile {
    uint8_t z;
    uint32_t x, y;
    std::vector<uint8_t> data;
};
std::deque<SdTile> sd_queue;
struct PendingBody {
    uint16_t conn;
    swp::TransferKind kind;
    std::vector<uint8_t> meta;
    std::vector<uint8_t> data;
};
std::deque<PendingBody> body_queue;  // HTTP bodies and long routes waiting for the transfer slot (worker-only)

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
    auto r = helper.get(url, auth_headers(), body, 16 * 1024);
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

int ms_since(int64_t t0) { return static_cast<int>((esp_timer_get_time() - t0) / 1000); }

// The helper's tile style names the SD cache tree (tiles from another style must not mix).
std::atomic<bool> style_known{false};
int64_t style_tried_us = INT64_MIN / 2;

void ensure_tile_style() {
    if (style_known.load() || !net::online() || !configured()) return;
    if (esp_timer_get_time() - style_tried_us < 30LL * 1000000) return;
    style_tried_us = esp_timer_get_time();
    std::vector<uint8_t> body;
    auto r = helper.get(server() + "/v1/status", auth_headers(), body, 4096);
    if (!r.ok()) return;
    body.push_back(0);
    cJSON* root = cJSON_Parse(reinterpret_cast<char*>(body.data()));
    std::string style = root ? str(root, "tile_style") : "";
    if (root) cJSON_Delete(root);
    storage::set_tile_style(style.empty() ? "1x" : style);  // helpers before tile_style rendered 1x
    style_known.store(true);
}

// SD cache first, then the helper. `from_net` tells the caller to cache the result.
bool load_tile(const swp::MapTileRequest& r, std::vector<uint8_t>& tile, bool& from_net) {
    from_net = false;
    if (r.z > 19 || r.x >= (1u << r.z) || r.y >= (1u << r.z)) return false;
    if (storage::tile_read(r.z, r.x, r.y, tile) && is_swt(tile)) return true;
    if (!net::online() || !configured()) return false;
    char url[200];
    snprintf(url, sizeof url, "%s/v1/maps/tile/%u/%lu/%lu.swt", server().c_str(), r.z, (unsigned long)r.x, (unsigned long)r.y);
    auto res = helper.get(url, auth_headers(), tile, kMaxTileBytes);
    if (!res.ok() || !is_swt(tile)) return false;
    from_net = true;
    return true;
}

// Fetches the next requested tile into `ready`.
void fetch_one() {
    ensure_tile_style();
    auto [conn, req] = tile_queue.front();
    tile_queue.pop_front();
    int64_t t0 = esp_timer_get_time();
    std::vector<uint8_t> data;
    bool from_net;
    if (!load_tile(req, data, from_net)) {
        send_error(conn, swp::ErrorCode::Unavailable, swp::MsgType::MapTileRequest, tile_meta_err(req));
        return;
    }
    ReadyTile t{conn, req, {}, ""};
    snprintf(t.label, sizeof t.label, "%u/%lu/%lu %s %d ms", req.z, (unsigned long)req.x, (unsigned long)req.y, from_net ? "net" : "sd",
             ms_since(t0));
    if (from_net && storage::mounted() && sd_queue.size() < 32) sd_queue.push_back({req.z, req.x, req.y, data});
    t.data = std::move(data);
    ready.push_back(std::move(t));
}

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

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
        r = helper.get(server() + "/v1/maps/data", auth_headers(), body, 4096);
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

// ---- HTTPS proxy for the watch ----------------------------------------------------------

bool header_ok(const std::string& s) { return s.find_first_of("\r\n") == std::string::npos; }

void do_http(const Job& j) {
    const swp::HttpRequest& q = *j.http;
    swp::HttpResponse out;
    out.id = q.id;
    static const esp_http_client_method_t methods[] = {HTTP_METHOD_GET,   HTTP_METHOD_POST,   HTTP_METHOD_PUT,
                                                       HTTP_METHOD_PATCH, HTTP_METHOD_DELETE, HTTP_METHOD_HEAD};
    http::Headers headers;
    bool valid = q.method < std::size(methods);
    for (const auto& h : q.headers) {
        valid = valid && !h.name.empty() && header_ok(h.name) && header_ok(h.value);
        headers.emplace_back(h.name, h.value);
    }
    if (!valid) {
        out.error = "bad request";
    } else if (!net::online()) {
        out.error = "companion is offline";
    } else {
        std::vector<uint8_t> body;
        size_t max = std::min<uint32_t>(q.max_response, swp::kHttpMaxResponse);
        auto r = http::request(methods[q.method], q.url, headers, q.body, body, max);
        ESP_LOGI(TAG, "proxy %u: status %d, %u bytes", q.id, r.status, (unsigned)body.size());
        out.status = static_cast<uint16_t>(r.status);
        out.error = r.error;
        out.body_size = static_cast<uint32_t>(body.size());
        if (!r.status) {
            out.body_size = 0;
        } else if (body.size() <= swp::kHttpInlineBody) {
            out.body = std::move(body);
        } else if (body_queue.size() < 4) {
            std::vector<uint8_t> meta;
            swp::Writer(meta).u16(q.id);
            body_queue.push_back({j.conn, swp::TransferKind::HttpBody, std::move(meta), std::move(body)});
        } else {
            out.status = 0;
            out.error = "companion busy";
            out.body_size = 0;
        }
    }
    ble_server::send(j.conn, out);
}

void start_transfer(uint16_t conn, swp::TransferKind kind, std::vector<uint8_t>&& data, uint32_t resume, std::vector<uint8_t> meta,
                    const char* label) {
    {
        std::lock_guard<std::mutex> g(mu);
        xfer.active = true;
        xfer.conn = conn;
        xfer.data = std::move(data);
        uint16_t mtu = ble_server::mtu(conn);
        // One chunk per notification: MTU - ATT(3) - frame(10) - chunk header(8).
        uint16_t chunk = static_cast<uint16_t>(std::clamp<int>(mtu - 21, 20, 480));
        xfer.tx.start(next_xfer_id++, kind, xfer.data.data(), static_cast<uint32_t>(xfer.data.size()), chunk, resume, std::move(meta));
        xfer.last_progress_us = xfer.started_us = esp_timer_get_time();
        strlcpy(xfer.label, label, sizeof xfer.label);
        ble_server::send(conn, xfer.tx.begin_message());
    }
    xTaskNotifyGive(xfer_task_handle);
}

void start_next_transfer() {
    // The queues are worker-only; xfer is shared with xfer_task and on_transfer_ack under `mu`.
    {
        std::lock_guard<std::mutex> g(mu);
        if (xfer.active) return;
    }
    if (!body_queue.empty()) {
        PendingBody b = std::move(body_queue.front());
        body_queue.pop_front();
        start_transfer(b.conn, b.kind, std::move(b.data), 0, std::move(b.meta), b.kind == swp::TransferKind::Route ? "route" : "http body");
    } else if (!ready.empty()) {
        ReadyTile t = std::move(ready.front());
        ready.pop_front();
        std::vector<uint8_t> meta;
        swp::Writer w(meta);
        w.u8(t.req.z);
        w.u32(t.req.x);
        w.u32(t.req.y);
        start_transfer(t.conn, swp::TransferKind::MapTile, std::move(t.data), t.req.resume_offset, std::move(meta), t.label);
    }
}

// Keeps the transfer slot busy while fetching ahead; returns early when other jobs wait.
void tiles_step() {
    start_next_transfer();
    while (ready.size() < kReadyAhead && !tile_queue.empty()) {
        fetch_one();
        start_next_transfer();
        if (uxQueueMessagesWaiting(jobs)) return;
    }
    if (tile_queue.empty() && ready.empty() && !sd_queue.empty()) {  // idle: cache what was fetched
        SdTile t = std::move(sd_queue.front());
        sd_queue.pop_front();
        storage::tile_write(t.z, t.x, t.y, t.data);
    }
}

void finish_transfer_locked(bool ok) {
    if (ok) {
        ESP_LOGI(TAG, "%s: %u B, ble %d ms", xfer.label, (unsigned)xfer.data.size(), ms_since(xfer.started_us));
    } else {
        ESP_LOGW(TAG, "%s: transfer timed out", xfer.label);
    }
    xfer.active = false;
    xfer.data.clear();
}

void pump_transfer() {
    std::lock_guard<std::mutex> g(mu);
    if (!xfer.active) return;
    if (xfer.tx.done() || esp_timer_get_time() - xfer.last_progress_us > 15LL * 1000000) {
        finish_transfer_locked(xfer.tx.done());
        return;
    }
    // Prioritise audio: while A2DP streams keep fewer frames in flight.
    int max_q = music::player::streaming() ? 2 : 8;
    while (ble_server::queued(xfer.conn) < max_q) {
        auto c = xfer.tx.next_chunk();
        if (!c) break;
        if (!ble_server::send(xfer.conn, *c)) break;
    }
}

void post(const Job& j);

void xfer_task(void*) {
    for (;;) {
        bool active;
        {
            std::lock_guard<std::mutex> g(mu);
            active = xfer.active;
        }
        if (!active) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            continue;
        }
        pump_transfer();
        bool done;
        {
            std::lock_guard<std::mutex> g(mu);
            done = !xfer.active;
        }
        if (done) {
            Job j = {};
            j.kind = JobKind::Wake;  // let the worker start the next transfer
            post(j);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void do_search(const Job& j) {
    if (!net::online() || !configured()) return send_error(j.conn, swp::ErrorCode::NoNetwork, swp::MsgType::MapSearchRequest, "offline");
    char url[320];
    std::string q = ccore::url_encode(j.query);
    snprintf(url, sizeof url, "%s/v1/maps/search?q=%s&lat=%.6f&lon=%.6f&limit=%u", server().c_str(), q.c_str(), j.lat_e7 / 1e7, j.lon_e7 / 1e7,
             j.limit);
    std::vector<uint8_t> body;
    auto r = helper.get(url, auth_headers(), body, 32 * 1024);
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
    auto r = helper.get(url, auth_headers(), body, 128 * 1024);
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
        if (rt.points.size() >= 1000 || !cJSON_IsArray(it) || cJSON_GetArraySize(it) < 2) continue;
        swp::RoutePoint p;
        p.lat_e7 = static_cast<int32_t>(std::lround(cJSON_GetArrayItem(it, 0)->valuedouble * 1e7));
        p.lon_e7 = static_cast<int32_t>(std::lround(cJSON_GetArrayItem(it, 1)->valuedouble * 1e7));
        rt.points.push_back(p);
    }
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(root, "steps")) {
        if (rt.steps.size() >= 100) break;
        swp::RouteStep s;
        s.instruction = str(it, "instruction");
        s.distance_m = static_cast<uint32_t>(num(it, "distance_m"));
        s.point_index = static_cast<uint16_t>(num(it, "point_index"));
        rt.steps.push_back(std::move(s));
    }
    cJSON_Delete(root);
    std::vector<uint8_t> payload = swp::encode_payload(rt);
    ESP_LOGI(TAG, "route: %u points, %u steps, %u B", (unsigned)rt.points.size(), (unsigned)rt.steps.size(), (unsigned)payload.size());
    if (payload.size() <= swp::kMaxPayload - 16) {
        ble_server::send(j.conn, rt);
    } else if (body_queue.size() < 4) {  // long trip: too big for one frame
        body_queue.push_back({j.conn, swp::TransferKind::Route, {}, std::move(payload)});
    } else {
        send_error(j.conn, swp::ErrorCode::Busy, swp::MsgType::RouteRequest, "companion busy");
    }
}

// ---- Background prefetch into the SD cache -------------------------------------------
// The SD cache (LRU, 4 GB) keeps every tile the watch has viewed. On top of that the
// companion keeps the area around its position downloaded, in one bundle response per
// refresh, so maps work offline without a manual download.

constexpr uint16_t kPrefetchRadiusM = 5000;
constexpr uint8_t kPrefetchZmin = 12, kPrefetchZmax = 16;
constexpr double kPrefetchMoveM = 2000;
constexpr int64_t kPrefetchMaxAgeS = 7 * 24 * 3600;


double distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
    double p = (static_cast<double>(lat1) + lat2) / 2e7 * M_PI / 180;
    double dx = (static_cast<double>(lon2) - lon1) / 1e7 * 111320 * std::cos(p), dy = (static_cast<double>(lat2) - lat1) / 1e7 * 110574;
    return std::sqrt(dx * dx + dy * dy);
}

// Splits a downloaded SWB1 bundle into the tile cache; returns the number of new tiles.
int store_bundle(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return -1;
    std::vector<char> iobuf(32 * 1024);  // records are tiny reads; without this each one hits the card
    setvbuf(f, iobuf.data(), _IOFBF, iobuf.size());
    uint8_t hdr[13];
    int stored = 0;
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
        if (is_swt(tile) && !storage::tile_exists(z, x, y) && storage::tile_write(z, x, y, tile)) ++stored;
        if (music::player::streaming()) vTaskDelay(pdMS_TO_TICKS(20));  // leave the card to the audio decoder
    }
    fclose(f);
    unlink(path.c_str());
    storage::flush_index();
    return ok ? stored : -1;
}

void prefetch_task(void*) {
    int32_t last_lat = 0, last_lon = 0;
    int64_t last_time = 0;
    bool loaded = false;
    std::string loaded_path;
    int64_t retry_after_us = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60 * 1000));
        swp::Position pos;
        int64_t now_us = esp_timer_get_time();
        if (now_us < retry_after_us || !net::online() || !configured() || !storage::mounted() || !position::current(pos)) continue;
        if (now_us - last_tile_request_us.load() < 20LL * 1000000 || music::player::streaming()) continue;  // stay out of the way
        std::string tiles = storage::tiles_dir();
        if (tiles.empty()) continue;  // the worker learns the tile style first
        std::string state_path = tiles + "/.prefetch";
        if (!loaded || state_path != loaded_path) {
            loaded_path = state_path;
            last_time = 0;
            std::vector<uint8_t> st;
            if (storage::read_file(state_path, st, 64)) {
                st.push_back(0);
                long la = 0, lo = 0;
                long long t = 0;
                unsigned rad = 0, zmax = 0;
                // Area settings are part of the state: changing them triggers a new download.
                if (sscanf(reinterpret_cast<char*>(st.data()), "%ld %ld %lld %u %u", &la, &lo, &t, &rad, &zmax) == 5 &&
                    rad == kPrefetchRadiusM && zmax == kPrefetchZmax) {
                    last_lat = static_cast<int32_t>(la);
                    last_lon = static_cast<int32_t>(lo);
                    last_time = t;
                }
            }
            loaded = true;
        }
        time_t now = time(nullptr);
        bool clock_ok = now > 1700000000;
        bool moved = last_time == 0 || distance_m(pos.lat_e7, pos.lon_e7, last_lat, last_lon) > kPrefetchMoveM;
        bool stale = clock_ok && now - last_time > kPrefetchMaxAgeS;
        if (!moved && !stale) continue;

        char url[256];
        snprintf(url, sizeof url, "%s/v1/maps/bundle?lat=%.6f&lon=%.6f&radius_m=%u&zmin=%u&zmax=%u", server().c_str(), pos.lat_e7 / 1e7,
                 pos.lon_e7 / 1e7, kPrefetchRadiusM, kPrefetchZmin, kPrefetchZmax);
        std::string path = std::string(storage::kMapsDir) + "/.bundle";
        uint64_t got = 0;
        int64_t t0 = esp_timer_get_time();
        auto r = http::download(url, auth_headers(), path, 0, 0, [] { return !net::online(); }, &got);
        if (!r.ok()) {
            unlink(path.c_str());
            ESP_LOGW(TAG, "prefetch: %s", r.status == 503 ? "helper is still building map data" : r.error.c_str());
            retry_after_us = esp_timer_get_time() + 10LL * 60 * 1000000;
            continue;
        }
        int dl_ms = ms_since(t0);
        int64_t t1 = esp_timer_get_time();
        int stored = store_bundle(path);
        ESP_LOGI(TAG, "prefetch: %llu KB in %d ms, %d new tiles stored in %d ms", (unsigned long long)(got / 1024), dl_ms, stored, ms_since(t1));
        if (stored < 0) {
            retry_after_us = esp_timer_get_time() + 10LL * 60 * 1000000;
            continue;
        }
        last_lat = pos.lat_e7;
        last_lon = pos.lon_e7;
        last_time = clock_ok ? now : 1;
        char st[64];
        int n = snprintf(st, sizeof st, "%ld %ld %lld %u %u", (long)last_lat, (long)last_lon, (long long)last_time, kPrefetchRadiusM,
                         kPrefetchZmax);
        storage::write_file_atomic(state_path, reinterpret_cast<uint8_t*>(st), n);
    }
}

void worker_task(void*) {
    Job j;
    for (;;) {
        bool busy = !tile_queue.empty() || !ready.empty() || !sd_queue.empty() || !body_queue.empty();
        TickType_t wait = pdMS_TO_TICKS(busy ? 20 : 1000);
        if (xQueueReceive(jobs, &j, wait) == pdTRUE) {
            switch (j.kind) {
                case JobKind::Tile: {
                    bool dup = std::any_of(tile_queue.begin(), tile_queue.end(), [&](const auto& q) {
                        return q.second.z == j.tile.z && q.second.x == j.tile.x && q.second.y == j.tile.y;
                    });
                    if (dup) break;
                    if (tile_queue.size() >= kMaxTileQueue) tile_queue.pop_front();  // the watch has moved on
                    tile_queue.emplace_back(j.conn, j.tile);
                    break;
                }
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
                case JobKind::MapData: do_map_data(j); break;
                case JobKind::Http:
                    do_http(j);
                    delete j.http;
                    break;
                case JobKind::Wake: break;
            }
        }
        static int64_t style_checked_us = 0;
        if (style_known.load() && esp_timer_get_time() - style_checked_us > 6LL * 3600 * 1000000) {
            style_checked_us = esp_timer_get_time();
            style_known.store(false);  // pick up a helper update within hours
        }
        if (net::online()) ensure_tile_style();
        tiles_step();

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

bool post_job(const Job& j) { return xQueueSend(jobs, &j, 0) == pdTRUE; }
void post(const Job& j) { post_job(j); }

void on_net(bool online) {
    if (online) {
        style_known.store(false);  // re-check after reconnecting (the helper may have been updated)
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
    xTaskCreatePinnedToCore(xfer_task, "xfer", 3072, nullptr, 5, &xfer_task_handle, 0);
    xTaskCreatePinnedToCore(prefetch_task, "prefetch", 6144, nullptr, 2, nullptr, 0);
}

void request_tile(uint16_t conn, const swp::MapTileRequest& r) {
    last_tile_request_us.store(esp_timer_get_time());
    Job j = {};
    j.kind = JobKind::Tile;
    j.conn = conn;
    j.tile = r;
    post(j);
}

void on_transfer_ack(uint16_t conn, const swp::TransferAck& a) {
    // Called from the BTC task; xfer_task sends the chunks the new credit allows, and wakes
    // the worker once the transfer is done.
    std::lock_guard<std::mutex> g(mu);
    if (xfer.active && xfer.conn == conn && xfer.tx.on_ack(a)) xfer.last_progress_us = esp_timer_get_time();
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

void request_map_data(uint16_t conn, const swp::MapDataCommand& c) {
    Job j = {};
    j.kind = JobKind::MapData;
    j.conn = conn;
    j.data = c;
    post(j);
}

void request_http(uint16_t conn, swp::HttpRequest&& r) {
    Job j = {};
    j.kind = JobKind::Http;
    j.conn = conn;
    j.http = new (std::nothrow) swp::HttpRequest(std::move(r));
    if (j.http && post_job(j)) return;
    swp::HttpResponse busy;
    busy.id = j.http ? j.http->id : r.id;
    busy.error = "companion busy";
    delete j.http;
    ble_server::send(conn, busy);
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
    // body_queue is worker-only; its entries fail fast in send() the same way.
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
