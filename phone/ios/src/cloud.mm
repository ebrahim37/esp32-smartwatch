// VPS helper client and map tile pipeline (port of firmware/companion/main/cloud.cpp), driven by
// callbacks on the main queue. Tiles are cached under Library/Caches.
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <iterator>
#include <map>

#include "app.hpp"
#include "ccore/quota.hpp"
#include "ccore/subsonic.hpp"
#include "ccore/sync_plan.hpp"
#include "swp/transfer.hpp"

namespace cloud {
namespace {

constexpr double kWeatherPeriodS = 15 * 60;
constexpr double kNotifResendS = 30;
constexpr size_t kMaxTileBytes = 200 * 1024;
constexpr size_t kMaxTileQueue = 24;
constexpr size_t kReadyAhead = 2;
constexpr size_t kMaxInFlight = 16;  // BLE writes queued in the app while a transfer runs

// Bumped when the link drops: replies still on their way belong to the old session.
uint32_t session = 0;

bool have_weather = false;
swp::Weather cached_weather;
double weather_fetched = -1e9, weather_tried = -1e9;
bool weather_fetching = false, weather_wanted = false;

struct PendingNotif {
    swp::Notification n;
    double sent = 0;  // 0 = not yet sent to the watch
};
std::map<uint64_t, PendingNotif> pending;

struct {
    bool active = false;
    std::vector<uint8_t> data;
    swp::TransferSender tx;
    double last_progress = 0;
} xfer;
uint16_t next_xfer_id = 1;

std::deque<swp::MapTileRequest> tile_queue;
struct ReadyTile {
    swp::MapTileRequest req;
    std::vector<uint8_t> data;
};
std::deque<ReadyTile> ready;
size_t tiles_inflight = 0;
struct PendingBody {
    swp::TransferKind kind;
    std::vector<uint8_t> meta;
    std::vector<uint8_t> data;
};
std::deque<PendingBody> body_queue;

std::string tile_style;  // names the cache tree: tiles of another style must not mix
bool style_known = false, style_fetching = false;
double style_tried = -1e9, style_checked = 0;
double last_tile_request = -1e9;

dispatch_queue_t io;  // tile files
std::atomic<uint64_t> maps_bytes{0};

http::Headers auth_headers() {
    http::Headers h;
    std::string tok = cfg::get(cfg::kServerToken);
    if (!tok.empty()) h.emplace_back("Authorization", "Bearer " + tok);
    return h;
}

std::string server() {
    std::string s = cfg::get(cfg::kServerUrl);
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

bool configured() { return !server().empty(); }

std::string fmt(const char* f, double a, double b) {
    char buf[96];
    snprintf(buf, sizeof buf, f, a, b);
    return buf;
}

http::Options helper_opts(size_t max) {
    http::Options o;
    o.headers = auth_headers();
    o.max_response = max;
    return o;
}

int16_t c10(double v) { return static_cast<int16_t>(std::lround(std::clamp(v, -300.0, 300.0) * 10)); }
uint16_t u10(double v) { return static_cast<uint16_t>(std::lround(std::clamp(v, 0.0, 6000.0) * 10)); }
uint8_t pct(double v) { return static_cast<uint8_t>(std::clamp(v, 0.0, 100.0)); }
int32_t e7(double v) { return static_cast<int32_t>(std::lround(std::clamp(v, -180.0, 180.0) * 1e7)); }

void send_error(swp::ErrorCode code, swp::MsgType ref, const std::string& msg) {
    swp::Error e;
    e.code = static_cast<uint8_t>(code);
    e.ref_type = static_cast<uint8_t>(ref);
    e.message = msg;
    ble::send(e);
}

// ---- Weather ----------------------------------------------------------------------------

void weather_reply(const http::Result& r) {
    net::set_server_ok(r.ok());
    id j = r.ok() ? jparse(r.body) : nil;
    if (![j isKindOfClass:NSDictionary.class]) return;
    swp::Weather w;
    w.updated_at = static_cast<uint32_t>(jnum(j, @"updated_at"));
    w.wmo_code = static_cast<uint8_t>(jnum(j, @"code"));
    w.description = jstr(j, @"description");
    w.temp_c10 = c10(jnum(j, @"temp_c"));
    w.low_c10 = c10(jnum(j, @"low_c"));
    w.high_c10 = c10(jnum(j, @"high_c"));
    w.wind_kmh10 = u10(jnum(j, @"wind_kmh"));
    w.uv10 = u10(jnum(j, @"uv"));
    id p = jval(j, @"precip");
    if ([p isKindOfClass:NSDictionary.class]) {
        std::string kind = jstr(p, @"kind");
        w.precip_kind = static_cast<uint8_t>(kind == "rain" ? swp::PrecipKind::Rain : kind == "snow" ? swp::PrecipKind::Snow
                                             : kind == "mixed" ? swp::PrecipKind::Mixed : kind == "none" ? swp::PrecipKind::None : swp::PrecipKind::Unknown);
        id m = jval(p, @"in_minutes");
        w.precip_in_min = [m isKindOfClass:NSNumber.class] ? [m intValue] : -1;
        w.precip_horizon_h = static_cast<uint8_t>(jnum(p, @"horizon_h"));
    }
    for (id it in jarr(j, @"days")) {
        if (w.days.size() >= 7) break;
        swp::WeatherDay d;
        d.date = static_cast<uint32_t>(jnum(it, @"date"));
        d.wmo_code = static_cast<uint8_t>(jnum(it, @"code"));
        d.low_c10 = c10(jnum(it, @"low_c"));
        d.high_c10 = c10(jnum(it, @"high_c"));
        d.precip_prob = pct(jnum(it, @"precip_prob"));
        d.precip_mm10 = static_cast<uint16_t>(std::clamp(jnum(it, @"precip_mm") * 10, 0.0, 65535.0));
        d.uv_max10 = static_cast<uint16_t>(std::clamp(jnum(it, @"uv_max") * 10, 0.0, 65535.0));
        w.days.push_back(d);
    }
    for (id it in jarr(j, @"hours")) {
        if (w.hours.size() >= 24) break;
        swp::WeatherHour h;
        h.time = static_cast<uint32_t>(jnum(it, @"time"));
        h.wmo_code = static_cast<uint8_t>(jnum(it, @"code"));
        h.temp_c10 = c10(jnum(it, @"temp_c"));
        h.precip_prob = pct(jnum(it, @"precip_prob"));
        w.hours.push_back(h);
    }
    cached_weather = w;
    have_weather = true;
    weather_fetched = mono_s();
    ble::send(w);
}

void fetch_weather() {
    swp::Position pos;
    if (weather_fetching || !position::current(pos) || !net::online() || !configured()) {
        if (weather_wanted && !weather_fetching && have_weather) ble::send(cached_weather);  // stale is better than nothing
        weather_wanted = false;
        return;
    }
    weather_fetching = true;
    weather_tried = mono_s();
    std::string url = server() + "/v1/weather?" + fmt("lat=%.5f&lon=%.5f", pos.lat_e7 / 1e7, pos.lon_e7 / 1e7);
    http::request(url, helper_opts(16 * 1024), [](http::Result r) {
        weather_fetching = false;
        weather_reply(r);
        if (weather_wanted && !r.ok() && have_weather) ble::send(cached_weather);
        weather_wanted = false;
    });
}

// ---- Notifications (at-least-once) ------------------------------------------------------

void deliver_pending() {
    if (!dispatch::watch_ready()) return;
    double now = mono_s();
    int sent = 0;
    for (auto& [id, p] : pending)
        if (p.sent == 0 || now - p.sent > kNotifResendS) {
            p.sent = now;
            ble::send(p.n);
            if (++sent >= 5) break;  // trickle: keep BLE free for interactive traffic
        }
}

// ---- Tile cache -------------------------------------------------------------------------

bool is_swt(const std::vector<uint8_t>& b) { return b.size() > 58 && memcmp(b.data(), "SWT1", 4) == 0; }

std::string style_dir(const std::string& style) { return style.empty() ? "" : paths::maps() + "/" + style; }

std::string tile_path(const std::string& style, uint8_t z, uint32_t x, uint32_t y) {
    return style_dir(style) + "/" + std::to_string(z) + "/" + std::to_string(x) + "/" + std::to_string(y) + ".swt";
}

bool read_file(const std::string& path, std::vector<uint8_t>& out, size_t max) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    bool ok = n >= 0 && static_cast<size_t>(n) <= max;
    if (ok) {
        out.resize(static_cast<size_t>(n));
        ok = fread(out.data(), 1, out.size(), f) == out.size();
    }
    fclose(f);
    return ok;
}

// io queue.
bool write_tile(const std::string& path, const uint8_t* data, size_t len) {
    NSString* p = ns(path);
    [NSFileManager.defaultManager createDirectoryAtPath:p.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    if (![[NSData dataWithBytesNoCopy:const_cast<uint8_t*>(data) length:len freeWhenDone:NO] writeToFile:p atomically:YES]) return false;
    maps_bytes += len;
    return true;
}

// io queue. Sums the cache and drops least recently used tiles above the quota.
void scan_and_evict() {
    struct Entry {
        std::string path;
        uint64_t size;
        time_t at;
    };
    std::vector<Entry> all;
    uint64_t total = 0;
    NSString* root = ns(paths::maps());
    NSDirectoryEnumerator* en = [NSFileManager.defaultManager enumeratorAtPath:root];
    for (NSString* rel in en) {
        if (![rel hasSuffix:@".swt"]) continue;
        std::string p = str([root stringByAppendingPathComponent:rel]);
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        total += st.st_size;
        all.push_back({std::move(p), static_cast<uint64_t>(st.st_size), st.st_mtime});
    }
    if (total > ccore::kMapsQuota) {
        std::sort(all.begin(), all.end(), [](const Entry& a, const Entry& b) { return a.at < b.at; });
        for (auto& e : all) {
            if (total <= ccore::kMapsQuota / 10 * 9) break;
            if (unlink(e.path.c_str()) == 0) total -= e.size;
        }
    }
    maps_bytes.store(total);
}

// ---- Transfers --------------------------------------------------------------------------

void start_transfer(swp::TransferKind kind, std::vector<uint8_t>&& data, uint32_t resume, std::vector<uint8_t> meta) {
    xfer.active = true;
    xfer.data = std::move(data);
    xfer.tx.start(next_xfer_id++, kind, xfer.data.data(), static_cast<uint32_t>(xfer.data.size()), ble::chunk_size(), resume, std::move(meta));
    xfer.last_progress = mono_s();
    ble::send(xfer.tx.begin_message());
}

void start_next_transfer() {
    if (xfer.active) return;
    if (!body_queue.empty()) {
        PendingBody b = std::move(body_queue.front());
        body_queue.pop_front();
        start_transfer(b.kind, std::move(b.data), 0, std::move(b.meta));
    } else if (!ready.empty()) {
        ReadyTile t = std::move(ready.front());
        ready.pop_front();
        std::vector<uint8_t> meta;
        swp::Writer w(meta);
        w.u8(t.req.z);
        w.u32(t.req.x);
        w.u32(t.req.y);
        start_transfer(swp::TransferKind::MapTile, std::move(t.data), t.req.resume_offset, std::move(meta));
    }
    pump();
}

void tiles_step();

void learn_style() {
    if (style_known || style_fetching || !net::online() || !configured() || mono_s() - style_tried < 30) return;
    style_fetching = true;
    style_tried = mono_s();
    http::request(server() + "/v1/status", helper_opts(4096), [](http::Result r) {
        style_fetching = false;
        if (r.ok()) {
            std::string s = jstr(jparse(r.body), @"tile_style");
            if (s.empty()) s = "1x";  // helpers before tile_style rendered 1x
            if (ccore::safe_id(s)) {
                tile_style = s;
                style_known = true;
                style_checked = mono_s();
            }
        }
        tiles_step();
    });
}

void tile_failed(const swp::MapTileRequest& r) {
    send_error(swp::ErrorCode::Unavailable, swp::MsgType::MapTileRequest, std::to_string(r.z) + "/" + std::to_string(r.x) + "/" + std::to_string(r.y));
}

void fetch_one() {
    swp::MapTileRequest req = tile_queue.front();
    tile_queue.pop_front();
    if (req.z > 19 || req.x >= (1u << req.z) || req.y >= (1u << req.z)) return tile_failed(req);
    std::vector<uint8_t> data;
    std::string path = style_known ? tile_path(tile_style, req.z, req.x, req.y) : "";
    if (!path.empty() && read_file(path, data, kMaxTileBytes) && is_swt(data)) {
        utimes(path.c_str(), nullptr);  // for LRU eviction
        ready.push_back({req, std::move(data)});
        return;
    }
    if (!net::online() || !configured()) return tile_failed(req);
    ++tiles_inflight;
    uint32_t sess = session;
    std::string url = server() + "/v1/maps/tile/" + std::to_string(req.z) + "/" + std::to_string(req.x) + "/" + std::to_string(req.y) + ".swt";
    http::request(url, helper_opts(kMaxTileBytes), [req, path, sess](http::Result r) {
        if (sess != session) return;
        --tiles_inflight;
        if (r.ok() && is_swt(r.body)) {
            if (!path.empty()) {
                auto copy = std::make_shared<std::vector<uint8_t>>(r.body);
                dispatch_async(io, ^{ write_tile(path, copy->data(), copy->size()); });
            }
            ready.push_back({req, std::move(r.body)});
        } else {
            tile_failed(req);
        }
        tiles_step();
    });
}

void tiles_step() {
    if (!style_known && !tile_queue.empty()) {
        learn_style();
        if (style_fetching) return;  // the cache location comes first
    }
    while (tiles_inflight + ready.size() < kReadyAhead && !tile_queue.empty()) fetch_one();
    start_next_transfer();
}

// ---- Search, route, map data, HTTP proxy --------------------------------------------------

void route_reply(const http::Result& r) {
    swp::Route rt;
    if (!r.ok()) {
        rt.status = static_cast<uint8_t>(swp::ErrorCode::Unavailable);
        ble::send(rt);
        return;
    }
    id root = jparse(r.body);
    rt.status = jstr(root, @"status") == "ok" ? 0 : static_cast<uint8_t>(swp::ErrorCode::NotFound);
    rt.distance_m = static_cast<uint32_t>(jnum(root, @"distance_m"));
    rt.duration_s = static_cast<uint32_t>(jnum(root, @"duration_s"));
    for (id it in jarr(root, @"points")) {
        if (rt.points.size() >= 1000) break;
        if (![it isKindOfClass:NSArray.class] || [it count] < 2 || ![it[0] isKindOfClass:NSNumber.class] || ![it[1] isKindOfClass:NSNumber.class]) continue;
        rt.points.push_back({e7([it[0] doubleValue]), e7([it[1] doubleValue])});
    }
    for (id it in jarr(root, @"steps")) {
        if (rt.steps.size() >= 100) break;
        swp::RouteStep s;
        s.instruction = jstr(it, @"instruction");
        s.distance_m = static_cast<uint32_t>(jnum(it, @"distance_m"));
        s.point_index = static_cast<uint16_t>(jnum(it, @"point_index"));
        rt.steps.push_back(std::move(s));
    }
    std::vector<uint8_t> payload = swp::encode_payload(rt);
    if (payload.size() <= swp::kMaxPayload - 16) {
        ble::send(rt);
    } else if (body_queue.size() < 4) {  // long trip: too big for one frame
        body_queue.push_back({swp::TransferKind::Route, {}, std::move(payload)});
        start_next_transfer();
    } else {
        send_error(swp::ErrorCode::Busy, swp::MsgType::RouteRequest, "phone busy");
    }
}

bool header_ok(const std::string& s) { return s.find_first_of("\r\n") == std::string::npos; }

// ---- Background prefetch of the surrounding area ------------------------------------------

constexpr unsigned kPrefetchRadiusM = 5000, kPrefetchZmin = 12, kPrefetchZmax = 16;
constexpr double kPrefetchMoveM = 2000;
constexpr int64_t kPrefetchMaxAgeS = 7 * 24 * 3600;
bool prefetching = false;
double prefetch_retry = 0;

double distance_m(int32_t lat1, int32_t lon1, int32_t lat2, int32_t lon2) {
    double p = (static_cast<double>(lat1) + lat2) / 2e7 * M_PI / 180;
    double dx = (static_cast<double>(lon2) - lon1) / 1e7 * 111320 * std::cos(p), dy = (static_cast<double>(lat2) - lat1) / 1e7 * 110574;
    return std::sqrt(dx * dx + dy * dy);
}

uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// io queue. Splits an SWB1 bundle into the cache; returns the number of new tiles, -1 on bad data.
int store_bundle(const std::string& path, const std::string& style) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return -1;
    uint8_t hdr[13];
    int stored = 0;
    bool ok = fread(hdr, 1, 4, f) == 4 && memcmp(hdr, "SWB1", 4) == 0;
    std::vector<uint8_t> tile;
    while (ok) {
        if (fread(hdr, 1, 1, f) != 1) { ok = false; break; }
        if (hdr[0] == 0xFF) break;
        if (fread(hdr + 1, 1, 12, f) != 12) { ok = false; break; }
        uint8_t z = hdr[0];
        uint32_t x = rd32(hdr + 1), y = rd32(hdr + 5), len = rd32(hdr + 9);
        if (z > 19 || len > kMaxTileBytes) { ok = false; break; }
        tile.resize(len);
        if (fread(tile.data(), 1, len, f) != len) { ok = false; break; }
        std::string p = tile_path(style, z, x, y);
        if (is_swt(tile) && access(p.c_str(), F_OK) != 0 && write_tile(p, tile.data(), tile.size())) ++stored;
    }
    fclose(f);
    unlink(path.c_str());
    return ok ? stored : -1;
}

void prefetch_tick() {
    swp::Position pos;
    if (prefetching || mono_s() < prefetch_retry || !net::online() || !configured() || !style_known || !position::current(pos)) return;
    if (mono_s() - last_tile_request < 20) return;  // stay out of the way
    std::string style = tile_style, state_key = "prefetch." + style;
    NSArray* st = [NSUserDefaults.standardUserDefaults arrayForKey:ns(state_key)];  // lat, lon, time, radius, zmax
    int64_t now = time(nullptr);
    bool known = st.count == 5 && [st[3] intValue] == kPrefetchRadiusM && [st[4] intValue] == kPrefetchZmax;
    bool moved = !known || distance_m(pos.lat_e7, pos.lon_e7, [st[0] intValue], [st[1] intValue]) > kPrefetchMoveM;
    bool stale = known && now - [st[2] longLongValue] > kPrefetchMaxAgeS;
    if (!moved && !stale) return;

    char q[128];
    snprintf(q, sizeof q, "/v1/maps/bundle?lat=%.6f&lon=%.6f&radius_m=%u&zmin=%u&zmax=%u", pos.lat_e7 / 1e7, pos.lon_e7 / 1e7, kPrefetchRadiusM, kPrefetchZmin, kPrefetchZmax);
    std::string path = paths::maps() + "/.bundle";
    http::Options o = helper_opts(512 * 1024 * 1024);
    o.timeout_s = 120;
    prefetching = true;
    http::download(server() + q, path, o, [path, style, state_key, pos, now](http::Result r) {
        if (!r.ok()) {
            prefetching = false;
            unlink(path.c_str());
            NSLog(@"prefetch: %s", r.status == 503 ? "helper is still building map data" : r.error.c_str());
            prefetch_retry = mono_s() + 600;
            return;
        }
        dispatch_async(io, ^{
            int stored = store_bundle(path, style);
            scan_and_evict();
            dispatch_async(dispatch_get_main_queue(), ^{
                prefetching = false;
                NSLog(@"prefetch: %d new tiles", stored);
                if (stored < 0) {
                    prefetch_retry = mono_s() + 600;
                    return;
                }
                [NSUserDefaults.standardUserDefaults setObject:@[@(pos.lat_e7), @(pos.lon_e7), @(now), @(kPrefetchRadiusM), @(kPrefetchZmax)] forKey:ns(state_key)];
            });
        });
    });
}

void tick() {
    double now = mono_s();
    if (xfer.active && !xfer.tx.done() && now - xfer.last_progress > 15) {
        NSLog(@"transfer timed out");
        xfer.active = false;
        xfer.data.clear();
        tiles_step();
    }
    pump();
    deliver_pending();
    if (style_known && now - style_checked > 6 * 3600) style_known = false;  // pick up a helper update within hours
    if (!style_known) learn_style();
    if (now - weather_fetched > kWeatherPeriodS && now - weather_tried > 60) fetch_weather();
    static uint32_t n = 0;
    if (++n % 60 == 0) prefetch_tick();
}

}  // namespace

void start() {
    io = dispatch_queue_create("sw.tiles", DISPATCH_QUEUE_SERIAL);
    dispatch_async(io, ^{ scan_and_evict(); });
    net::add_listener([] {
        if (net::online()) style_known = false;  // re-check after reconnecting
        tick();
    });
    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer*) { tick(); }];
}

void pump() {
    if (!xfer.active) return;
    if (xfer.tx.done()) {
        xfer.active = false;
        xfer.data.clear();
        tiles_step();
        return;
    }
    while (ble::queued() < kMaxInFlight) {
        auto c = xfer.tx.next_chunk();
        if (!c || !ble::send(*c)) break;
    }
}

void request_tile(const swp::MapTileRequest& r) {
    last_tile_request = mono_s();
    bool dup = std::any_of(tile_queue.begin(), tile_queue.end(), [&](const auto& q) { return q.z == r.z && q.x == r.x && q.y == r.y; });
    if (!dup) {
        if (tile_queue.size() >= kMaxTileQueue) tile_queue.pop_front();  // the watch has moved on
        tile_queue.push_back(r);
    }
    tiles_step();
}

void on_transfer_ack(const swp::TransferAck& a) {
    if (xfer.active && xfer.tx.on_ack(a)) xfer.last_progress = mono_s();
    pump();
}

void request_search(const swp::MapSearchRequest& m) {
    if (!net::online() || !configured()) return send_error(swp::ErrorCode::NoNetwork, swp::MsgType::MapSearchRequest, "offline");
    unsigned limit = m.limit ? std::min<unsigned>(m.limit, 12) : 10;
    std::string url = server() + "/v1/maps/search?q=" + ccore::url_encode(m.query) + "&" + fmt("lat=%.6f&lon=%.6f", m.lat_e7 / 1e7, m.lon_e7 / 1e7) + "&limit=" + std::to_string(limit);
    std::string query = m.query;
    http::request(url, helper_opts(32 * 1024), [query](http::Result r) {
        if (!r.ok()) return send_error(swp::ErrorCode::Unavailable, swp::MsgType::MapSearchRequest, "search failed");
        swp::MapSearchResults res;
        res.query = query;
        for (id it in jarr(jparse(r.body), @"places")) {
            if (res.places.size() >= 12) break;
            swp::Place p;
            p.name = jstr(it, @"name");
            p.category = jstr(it, @"category");
            p.address = jstr(it, @"address");
            p.lat_e7 = e7(jnum(it, @"lat"));
            p.lon_e7 = e7(jnum(it, @"lon"));
            p.distance_m = static_cast<uint32_t>(jnum(it, @"distance_m"));
            res.places.push_back(std::move(p));
        }
        ble::send(res);
    });
}

void request_route(const swp::RouteRequest& q) {
    if (!net::online() || !configured()) return send_error(swp::ErrorCode::NoNetwork, swp::MsgType::RouteRequest, "offline");
    static const char* modes[] = {"walk", "bike", "drive"};
    std::string url = server() + "/v1/maps/route?" + fmt("from=%.6f,%.6f", q.from_lat_e7 / 1e7, q.from_lon_e7 / 1e7) + "&" +
                      fmt("to=%.6f,%.6f", q.to_lat_e7 / 1e7, q.to_lon_e7 / 1e7) + "&mode=" + modes[q.mode % 3];
    http::request(url, helper_opts(128 * 1024), [](http::Result r) { route_reply(r); });
}

void request_weather() {
    if (have_weather && mono_s() - weather_fetched < kWeatherPeriodS) {
        ble::send(cached_weather);
        return;
    }
    weather_wanted = true;
    fetch_weather();
}

void request_map_data(const swp::MapDataCommand& c) {
    if (!net::online() || !configured()) {
        swp::MapDataStatus out;
        out.status = static_cast<uint8_t>(swp::ErrorCode::NoNetwork);
        out.error = "Phone is offline";
        ble::send(out);
        return;
    }
    auto action = static_cast<swp::MapDataAction>(c.action);
    http::Options o = helper_opts(4096);
    std::string url = server() + "/v1/maps/data";
    if (action != swp::MapDataAction::Status) {
        o.method = "POST";
        o.headers.emplace_back("Content-Type", "application/json");
        std::string json = "{}";
        if (action == swp::MapDataAction::Update) json = fmt("{\"lat\":%.6f,\"lon\":%.6f}", c.lat_e7 / 1e7, c.lon_e7 / 1e7);
        o.body.assign(json.begin(), json.end());
        url += action == swp::MapDataAction::Update ? "/update" : "/delete";
    }
    http::request(url, o, [](http::Result r) {
        swp::MapDataStatus out;
        id root = jparse(r.body);
        if (!r.ok()) {
            out.status = static_cast<uint8_t>(swp::ErrorCode::Unavailable);
            out.error = jstr(root, @"error");
            if (out.error.empty()) out.error = r.error.empty() ? "HTTP " + std::to_string(r.status) : r.error;
        } else {
            out.active = jstr(root, @"active");
            out.building = jstr(root, @"building");
            out.error = jstr(root, @"last_error");
        }
        if (out.error.size() > 300) out.error.resize(300);
        ble::send(out);
    });
}

void request_http(swp::HttpRequest q) {
    swp::HttpResponse out;
    out.id = q.id;
    static const char* methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};
    bool valid = q.method < std::size(methods) && q.url.rfind("https://", 0) == 0;
    http::Options o;
    for (const auto& h : q.headers) {
        valid = valid && !h.name.empty() && header_ok(h.name) && header_ok(h.value);
        o.headers.emplace_back(h.name, h.value);
    }
    if (!valid) {
        out.error = "bad request";
        ble::send(out);
        return;
    }
    if (!net::online()) {
        out.error = "phone is offline";
        ble::send(out);
        return;
    }
    o.method = methods[q.method];
    o.body = q.body;
    o.max_response = std::min<uint32_t>(q.max_response, swp::kHttpMaxResponse);
    uint16_t id = q.id;
    uint32_t sess = session;
    http::request(q.url, o, [id, sess](http::Result r) {
        if (sess != session) return;
        swp::HttpResponse out;
        out.id = id;
        out.status = static_cast<uint16_t>(r.status);
        out.error = r.error.substr(0, 200);
        out.body_size = static_cast<uint32_t>(r.body.size());
        bool queue_body = false;
        if (!r.status) {
            out.body_size = 0;
        } else if (r.body.size() <= swp::kHttpInlineBody) {
            out.body = r.body;
        } else if (body_queue.size() < 4) {
            queue_body = true;
        } else {
            out.status = 0;
            out.error = "phone busy";
            out.body_size = 0;
        }
        ble::send(out);
        if (queue_body) {
            std::vector<uint8_t> meta;
            swp::Writer(meta).u16(id);
            body_queue.push_back({swp::TransferKind::HttpBody, std::move(meta), std::move(r.body)});
            start_next_transfer();
        }
    });
}

void on_watch_connected() {
    for (auto& [id, p] : pending) p.sent = 0;  // redeliver everything not acknowledged
    deliver_pending();
}

void on_watch_disconnected() {
    ++session;
    xfer.active = false;
    xfer.data.clear();
    tile_queue.clear();
    ready.clear();
    body_queue.clear();
    tiles_inflight = 0;
}

bool weather(swp::Weather& out) {
    if (have_weather) out = cached_weather;
    return have_weather;
}

void ingest_notification(const swp::Notification& n) {
    if (pending.count(n.id) || pending.size() >= 64) return;
    pending.emplace(n.id, PendingNotif{n});
    deliver_pending();
}

void on_notification_ack(uint64_t id) {
    pending.erase(id);
    ntfy::on_ack(id);
}

uint16_t pending_notifications() { return static_cast<uint16_t>(pending.size()); }
uint64_t maps_used() { return maps_bytes.load(); }

}  // namespace cloud
