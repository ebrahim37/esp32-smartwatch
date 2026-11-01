// Maps: Apple-Watch-style full-screen map with location marker, relative heading, zoom,
// immediate nearby search, results, place details and route display.
//
// Map imagery: 256 px SWT tiles (16-colour, zlib) rendered/re-coloured by the VPS helper from
// self-built OSM data, cached on the companion's SD card and streamed over BLE. Each visible
// slot inflates its tile once into an RGB565 buffer that LVGL draws directly.
#include <cmath>

#include "app/services.hpp"
#include "app/state.hpp"
#include "app/tiles.hpp"
#include "ble/central.hpp"
#include "ble/link.hpp"
#include "core/map_math.hpp"
#include "esp_heap_caps.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "src/misc/cache/instance/lv_image_header_cache.h"
#include "ui/ui.hpp"

namespace ui {
namespace {

constexpr int kW = 410, kH = 502;
constexpr int kGridCols = 3, kGridRows = 4;
constexpr int kMaxRoutePts = 600;  // drawn vertices (the visible stretch of the route)

struct TileSlot {
    tiles::Key key{};
    tiles::Blob blob;
    lv_image_dsc_t* dsc = nullptr;
    lv_obj_t* img = nullptr;
    uint16_t* px = nullptr;  // 256x256 RGB565 in PSRAM, owned by the slot
};

void release_slot(TileSlot& s) {
    if (s.dsc) {
        lv_image_cache_drop(s.dsc);  // decoded pixels are keyed by the descriptor address
        lv_image_header_cache_drop(s.dsc);
        delete s.dsc;
        s.dsc = nullptr;
    }
    s.blob.reset();
}

class MapScreen : public Screen {
public:
    bool has_top_bar() const override { return false; }
    bool scrollable() const override { return false; }
    uint32_t tick_ms() const override { return 300; }

    ~MapScreen() override {
        central::set_fast(false);
        for (auto& s : slots_) {
            release_slot(s);
            heap_caps_free(s.px);
        }
    }

    void build(lv_obj_t* c) override {
        central::set_fast(true);
        lv_obj_set_style_bg_color(c, lv_color_hex(0x101214), 0);
        layer_ = lv_obj_create(c);
        lv_obj_remove_style_all(layer_);
        lv_obj_set_size(layer_, kW, kH);
        lv_obj_add_flag(layer_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(layer_, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE));
        lv_obj_add_event_cb(layer_, on_drag, LV_EVENT_PRESSING, this);
        lv_obj_add_event_cb(layer_, on_release, LV_EVENT_RELEASED, this);
        for (auto& s : slots_) {
            s.img = lv_image_create(layer_);
            lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s.img, LV_OBJ_FLAG_CLICKABLE);
        }
        for (auto& l : route_) {
            l = lv_line_create(layer_);
            lv_obj_set_style_line_width(l, 7, 0);
            lv_obj_set_style_line_color(l, color::blue(), 0);
            lv_obj_set_style_line_rounded(l, true, 0);
            lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        }

        // Heading cone (relative, gyro-based) + location dot.
        cone_ = lv_arc_create(layer_);
        lv_obj_set_size(cone_, 92, 92);
        lv_arc_set_bg_angles(cone_, 0, 0);
        lv_obj_set_style_arc_opa(cone_, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(cone_, 30, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(cone_, color::blue(), LV_PART_INDICATOR);
        lv_obj_set_style_arc_opa(cone_, LV_OPA_50, LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(cone_, false, LV_PART_INDICATOR);
        lv_obj_remove_style(cone_, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(cone_, LV_OBJ_FLAG_CLICKABLE);
        dot_ = lv_obj_create(layer_);
        lv_obj_set_size(dot_, 24, 24);
        lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot_, color::blue(), 0);
        lv_obj_set_style_border_color(dot_, color::text(), 0);
        lv_obj_set_style_border_width(dot_, 4, 0);
        lv_obj_remove_flag(dot_, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        dest_ = lv_obj_create(layer_);
        lv_obj_set_size(dest_, 22, 22);
        lv_obj_set_style_radius(dest_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dest_, color::red(), 0);
        lv_obj_set_style_border_color(dest_, color::text(), 0);
        lv_obj_set_style_border_width(dest_, 3, 0);
        lv_obj_remove_flag(dest_, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        lv_obj_add_flag(dest_, LV_OBJ_FLAG_HIDDEN);

        status_ = label(c, "", &font_inter_16, color::muted());
        lv_obj_align(status_, LV_ALIGN_CENTER, 0, 0);

        // Floating controls (back/home like every screen, zoom, search, recenter).
        place_nav(round_button(c, LV_SYMBOL_LEFT, lv_color_hex(0x2C2C2E), kNavBtn, [] { pop(); }), false);
        place_nav(round_button(c, LV_SYMBOL_HOME, lv_color_hex(0x2C2C2E), kNavBtn, [] { go_home(); }), true);
        lv_obj_t* plus = round_button(c, LV_SYMBOL_PLUS, lv_color_hex(0x2C2C2E), 64, [this] { zoom(+1); });
        lv_obj_align(plus, LV_ALIGN_RIGHT_MID, -14, -40);
        lv_obj_t* minus = round_button(c, LV_SYMBOL_MINUS, lv_color_hex(0x2C2C2E), 64, [this] { zoom(-1); });
        lv_obj_align(minus, LV_ALIGN_RIGHT_MID, -14, 36);
        lv_obj_t* loc = round_button(c, LV_SYMBOL_GPS, lv_color_hex(0x2C2C2E), 64, [this] { recenter(); });
        lv_obj_align(loc, LV_ALIGN_LEFT_MID, 14, 36);  // level with zoom -
        lv_obj_t* search = round_button(c, UI_SYMBOL_SEARCH, lv_color_hex(0x2C2C2E), 64, [this] { open_search(); });
        lv_obj_align(search, LV_ALIGN_LEFT_MID, 14, -40);  // level with zoom +

        banner_ = card(c);
        lv_obj_set_size(banner_, 340, LV_SIZE_CONTENT);
        lv_obj_align(banner_, LV_ALIGN_BOTTOM_MID, 0, -30);
        lv_obj_set_style_bg_color(banner_, lv_color_hex(0x1C1C1E), 0);
        banner_lbl_ = label(banner_, "", &font_inter_20);
        lv_obj_set_width(banner_lbl_, 210);
        lv_label_set_long_mode(banner_lbl_, LV_LABEL_LONG_WRAP);
        lv_obj_t* end = pill_button(banner_, "End", color::red(), [] {
            app::Locked s;
            s->route_valid = false;
            app::mark(app::kDirtyRoute);
        }, 90, 56);
        lv_obj_align(end, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);

        lv_obj_t* attr = label(c, "\xC2\xA9 OpenStreetMap contributors", &font_inter_16, color::muted());
        lv_obj_set_style_text_font(attr, &font_inter_16, 0);
        lv_obj_align(attr, LV_ALIGN_BOTTOM_MID, 0, -10);

        // TODO(PROTOTYPE): the heading starts at 0 deg ("north") every time Maps opens and is
        // integrated from the gyro only; it is labelled REL and never shown as a compass.
        services::reset_heading();
        recenter();
    }

    void refresh(uint32_t d) override {
        if (d & app::kDirtyPosition) {
            if (follow_) recenter();
            else layout();
        }
        if (d & app::kDirtyTiles) layout();
        if (d & app::kDirtyRoute) update_route();
        if (d & app::kDirtyLink) layout();
    }

    void tick() override {
        float h = services::heading_deg();
        // LVGL angles: 0 = east, clockwise. Heading 0 = up.
        int a = static_cast<int>(h) - 90;
        lv_arc_set_angles(cone_, static_cast<lv_value_precise_t>((a - 28 + 720) % 360), static_cast<lv_value_precise_t>((a + 28 + 720) % 360));
        if (++ticks_ % 10 == 0) tiles::pump();  // retry timed-out requests
    }

private:
    static void on_drag(lv_event_t* e) {
        auto* self = static_cast<MapScreen*>(lv_event_get_user_data(e));
        lv_point_t v;
        lv_indev_get_vect(lv_indev_active(), &v);
        if (v.x == 0 && v.y == 0) return;
        self->follow_ = false;
        self->center_.x -= v.x;
        self->center_.y -= v.y;
        self->layout(false);
    }
    static void on_release(lv_event_t* e) { static_cast<MapScreen*>(lv_event_get_user_data(e))->layout(true); }

    bool position(double& lat, double& lon) {
        app::Locked s;
        if (!s->position_valid) return false;
        lat = core::e7_to_deg(s->position.lat_e7);
        lon = core::e7_to_deg(s->position.lon_e7);
        return true;
    }

    void recenter() {
        follow_ = true;
        double lat, lon;
        if (!position(lat, lon)) {
            // TODO(PROTOTYPE): same dummy fix the companion reports, until it arrives.
            lat = 43 + 46.0 / 60 + 48.3 / 3600;
            lon = -(79 + 24.0 / 60 + 42.7 / 3600);
        }
        center_ = core::latlon_to_px(lat, lon, zoom_);
        layout();
    }

    void zoom(int dz) {
        int nz = std::clamp(zoom_ + dz, core::kMinZoom, core::kMaxZoom);
        if (nz == zoom_) return;
        double lat, lon;
        core::px_to_latlon(center_, zoom_, lat, lon);
        zoom_ = nz;
        center_ = core::latlon_to_px(lat, lon, zoom_);
        layout();
        update_route();
    }

    lv_point_t to_screen(int32_t lat_e7, int32_t lon_e7) const {
        auto p = core::latlon_to_px(core::e7_to_deg(lat_e7), core::e7_to_deg(lon_e7), zoom_);
        return {static_cast<int32_t>(p.x - center_.x + kW / 2), static_cast<int32_t>(p.y - center_.y + kH / 2)};
    }

    void layout(bool request = true) {
        int64_t left = static_cast<int64_t>(center_.x) - kW / 2, top = static_cast<int64_t>(center_.y) - kH / 2;
        int64_t tx0 = left >= 0 ? left / 256 : (left - 255) / 256, ty0 = top >= 0 ? top / 256 : (top - 255) / 256;
        int64_t n = 1ll << zoom_;
        std::vector<tiles::Key> want;
        int missing = 0;
        int i = 0;
        for (int r = 0; r < kGridRows; ++r)
            for (int col = 0; col < kGridCols; ++col, ++i) {
                TileSlot& s = slots_[i];
                int64_t tx = tx0 + col, ty = ty0 + r;
                int sx = static_cast<int>(tx * 256 - left), sy = static_cast<int>(ty * 256 - top);
                bool visible = ty >= 0 && ty < n && sx < kW && sy < kH && sx > -256 && sy > -256;
                if (!visible) {
                    lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                    continue;
                }
                tiles::Key k{static_cast<uint8_t>(zoom_), static_cast<uint32_t>(((tx % n) + n) % n), static_cast<uint32_t>(ty)};
                if (!(s.key == k) || !s.blob) {
                    auto blob = tiles::get(k);
                    if (!(s.key == k) || blob) {
                        lv_image_set_src(s.img, nullptr);
                        release_slot(s);
                        s.key = k;
                        if (!s.px) s.px = static_cast<uint16_t*>(heap_caps_malloc(256 * 256 * 2, MALLOC_CAP_SPIRAM));
                        if (blob && s.px && tiles::decode(blob, s.px)) {
                            s.blob = blob;
                            s.dsc = new lv_image_dsc_t{};
                            s.dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
                            s.dsc->header.cf = LV_COLOR_FORMAT_RGB565;
                            s.dsc->header.w = 256;
                            s.dsc->header.h = 256;
                            s.dsc->header.stride = 256 * 2;
                            s.dsc->data_size = 256 * 256 * 2;
                            s.dsc->data = reinterpret_cast<const uint8_t*>(s.px);
                            lv_image_set_src(s.img, s.dsc);
                        }
                    }
                }
                if (s.blob) {
                    lv_obj_remove_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                    lv_obj_set_pos(s.img, sx, sy);
                } else {
                    lv_obj_add_flag(s.img, LV_OBJ_FLAG_HIDDEN);
                    ++missing;
                    want.push_back(k);
                }
            }
        // Nearest-to-centre first.
        std::sort(want.begin(), want.end(), [&](const tiles::Key& a, const tiles::Key& b) {
            auto d = [&](const tiles::Key& k) {
                double cx = k.x * 256.0 + 128 - center_.x, cy = k.y * 256.0 + 128 - center_.y;
                return cx * cx + cy * cy;
            };
            return d(a) < d(b);
        });
        if (request && !want.empty()) tiles::want(want);

        bool ready;
        bool audio;
        {
            app::Locked s;
            ready = s->link.state == app::LinkState::Ready;
            audio = s->link.peer_audio_active;
        }
        if (missing > 0 && !ready) set_text(status_, "Companion offline: showing cached map only");
        else if (missing > 0) set_text(status_, audio ? "Loading map (slower while music plays)..." : "Loading map...");
        else set_text(status_, "");

        double lat, lon;
        if (!position(lat, lon)) {
            lat = 43 + 46.0 / 60 + 48.3 / 3600;
            lon = -(79 + 24.0 / 60 + 42.7 / 3600);
        }
        lv_point_t me = to_screen(core::deg_to_e7(lat), core::deg_to_e7(lon));
        lv_obj_set_pos(dot_, me.x - 12, me.y - 12);
        lv_obj_set_pos(cone_, me.x - 46, me.y - 46);
        update_route_points();
    }

    void update_route() {
        bool valid;
        std::string text;
        {
            app::Locked s;
            valid = s->route_valid;
            if (valid) {
                char b[160];
                const auto& r = s->route;
                std::string first = r.steps.empty() ? "" : r.steps.front().instruction;
                snprintf(b, sizeof b, "%.1f km \xC2\xB7 %u min\n%s", r.distance_m / 1000.0, static_cast<unsigned>((r.duration_s + 59) / 60),
                         first.c_str());
                text = b;
                route_pts_.clear();
                for (auto& p : r.points) route_pts_.push_back(p);
            } else if (s->route_pending) {
                text = "Finding route...";
            }
        }
        if (valid || !text.empty()) {
            set_text(banner_lbl_, text);
            lv_obj_remove_flag(banner_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
            route_pts_.clear();
        }
        update_route_points();
    }

    void update_route_points() {
        if (route_pts_.size() < 2) {
            for (auto l : route_) lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(dest_, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        // Clip every segment to the screen plus a margin: a 100+ km route at street zoom would
        // otherwise produce coordinates far off-screen and an enormous line object.
        constexpr float kM = 64, x0 = -kM, y0 = -kM, x1 = kW + kM, y1 = kH + kM;
        auto clip = [&](float& ax, float& ay, float& bx, float& by) {  // Liang-Barsky
            float t0 = 0, t1 = 1, dx = bx - ax, dy = by - ay;
            const float p[4] = {-dx, dx, -dy, dy}, q[4] = {ax - x0, x1 - ax, ay - y0, y1 - ay};
            for (int i = 0; i < 4; ++i) {
                if (p[i] == 0) {
                    if (q[i] < 0) return false;
                } else {
                    float t = q[i] / p[i];
                    if (p[i] < 0) t0 = std::max(t0, t);
                    else t1 = std::min(t1, t);
                }
            }
            if (t0 > t1) return false;
            bx = ax + t1 * dx, by = ay + t1 * dy;
            ax = ax + t0 * dx, ay = ay + t0 * dy;
            return true;
        };
        // Each on-screen stretch of the route is its own line (lv_line has no gaps).
        size_t n = 0, runs = 0, run_start = 0;
        auto close_run = [&] {
            if (n - run_start >= 2 && runs < kRouteRuns) {
                lv_line_set_points(route_[runs], line_pts_ + run_start, static_cast<uint32_t>(n - run_start));
                lv_obj_set_pos(route_[runs], 0, 0);
                lv_obj_remove_flag(route_[runs], LV_OBJ_FLAG_HIDDEN);
                ++runs;
                run_start = n;
            } else {
                n = run_start;  // too short (or out of line objects): drop it
            }
        };
        auto emit = [&](float x, float y) {
            lv_point_precise_t p{static_cast<lv_value_precise_t>(std::lround(x)), static_cast<lv_value_precise_t>(std::lround(y))};
            if (n > run_start && line_pts_[n - 1].x == p.x && line_pts_[n - 1].y == p.y) return;
            if (n < static_cast<size_t>(kMaxRoutePts)) line_pts_[n++] = p;
        };
        lv_point_t prev = to_screen(route_pts_[0].lat_e7, route_pts_[0].lon_e7);
        bool in_run = false;
        for (size_t i = 1; i < route_pts_.size(); ++i) {
            lv_point_t cur = to_screen(route_pts_[i].lat_e7, route_pts_[i].lon_e7);
            float ax = prev.x, ay = prev.y, bx = cur.x, by = cur.y;
            bool visible = clip(ax, ay, bx, by);
            bool entered = ax != prev.x || ay != prev.y;  // segment started off-screen
            if (in_run && (!visible || entered)) close_run();
            if (visible) {
                emit(ax, ay);
                emit(bx, by);
            }
            in_run = visible && bx == cur.x && by == cur.y;  // still on screen at its end
            if (visible && !in_run) close_run();
            prev = cur;
        }
        if (in_run) close_run();
        for (size_t i = runs; i < kRouteRuns; ++i) lv_obj_add_flag(route_[i], LV_OBJ_FLAG_HIDDEN);
        lv_point_t d = to_screen(route_pts_.back().lat_e7, route_pts_.back().lon_e7);
        lv_obj_set_pos(dest_, d.x - 11, d.y - 11);
        lv_obj_remove_flag(dest_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(dot_);
    }

    void open_search();

    static constexpr size_t kRouteRuns = 6;
    lv_obj_t* route_[kRouteRuns] = {};
    lv_obj_t *layer_ = nullptr, *cone_ = nullptr, *dot_ = nullptr, *dest_ = nullptr;
    lv_obj_t *status_ = nullptr, *banner_ = nullptr, *banner_lbl_ = nullptr;
    TileSlot slots_[kGridCols * kGridRows];
    core::WorldPx center_{0, 0};
    int zoom_ = 15;
    bool follow_ = true;
    uint32_t ticks_ = 0;
    std::vector<swp::RoutePoint> route_pts_;
    lv_point_precise_t line_pts_[kMaxRoutePts];
};

void send_search(const std::string& q) {
    swp::MapSearchRequest r;
    r.query = q;
    r.limit = 10;
    {
        app::Locked s;
        if (s->position_valid) {
            r.lat_e7 = s->position.lat_e7;
            r.lon_e7 = s->position.lon_e7;
        }
        s->search_pending = true;
        s->search = swp::MapSearchResults{};
    }
    app::mark(app::kDirtySearch);
    if (!swlink::send(r)) {
        app::Locked s;
        s->search_pending = false;
        s->last_error_text = "Companion not connected";
    }
}

class PlaceScreen : public Screen {
public:
    explicit PlaceScreen(swp::Place p) : p_(std::move(p)) {}
    const char* title() const override { return p_.name.c_str(); }
    void build(lv_obj_t* c) override {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 16, 0);
        lv_obj_set_style_pad_row(c, 10, 0);
        label(c, p_.category.c_str(), &font_inter_20, color::muted());
        lv_obj_t* a = label(c, p_.address.c_str(), &font_inter_20);
        lv_obj_set_width(a, LV_PCT(100));
        lv_label_set_long_mode(a, LV_LABEL_LONG_WRAP);
        char b[32];
        if (p_.distance_m >= 1000) snprintf(b, sizeof b, "%.1f km away", p_.distance_m / 1000.0);
        else snprintf(b, sizeof b, "%u m away", static_cast<unsigned>(p_.distance_m));
        label(c, b, &font_inter_20, color::muted());
        swp::Place p = p_;
        auto go = [p](swp::TravelMode m) {
            swp::RouteRequest r;
            {
                app::Locked s;
                if (s->position_valid) {
                    r.from_lat_e7 = s->position.lat_e7;
                    r.from_lon_e7 = s->position.lon_e7;
                }
                s->route_pending = true;
                s->route_valid = false;
            }
            r.to_lat_e7 = p.lat_e7;
            r.to_lon_e7 = p.lon_e7;
            r.mode = static_cast<uint8_t>(m);
            swlink::send(r);
            app::mark(app::kDirtyRoute);
            pop_n(2);  // back to the map (this screen and the results list)
        };
        pill_button(c, "Walk", color::blue(), [go] { go(swp::TravelMode::Walk); }, LV_PCT(100), 64);
        pill_button(c, "Cycle", color::card2(), [go] { go(swp::TravelMode::Bike); }, LV_PCT(100), 64);
        pill_button(c, "Drive", color::card2(), [go] { go(swp::TravelMode::Drive); }, LV_PCT(100), 64);
    }

private:
    swp::Place p_;
};

class SearchScreen : public Screen {
public:
    const char* title() const override { return "Nearby"; }
    void build(lv_obj_t* c) override {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_row(c, 8, 0);
        list_row(c, "Search places", "Tap to type", nullptr, [] {
            KeyboardOpts o;
            o.label = "Search";
            o.max_len = 60;
            open_keyboard(o, [](const std::string& q) { send_search(q); });
        }, color::blue(), LV_SYMBOL_KEYBOARD);
        results_ = lv_obj_create(c);
        lv_obj_remove_style_all(results_);
        lv_obj_set_size(results_, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(results_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(results_, 8, 0);
        refresh(0xFFFFFFFFu);
    }
    void refresh(uint32_t d) override {
        if (!(d & app::kDirtySearch)) return;
        clear(results_);
        swp::MapSearchResults r;
        bool pending;
        std::string err;
        {
            app::Locked s;
            r = s->search;
            pending = s->search_pending;
            err = s->last_error_text;
        }
        if (pending) {
            section_header(results_, "Searching...");
            return;
        }
        if (r.places.empty()) {
            section_header(results_, err.empty() ? "No results" : err.c_str());
            return;
        }
        section_header(results_, r.query.empty() ? "Nearby" : ("Results for \"" + r.query + "\"").c_str());
        for (auto& p : r.places) {
            char dist[24];
            if (p.distance_m >= 1000) snprintf(dist, sizeof dist, "%.1f km", p.distance_m / 1000.0);
            else snprintf(dist, sizeof dist, "%u m", static_cast<unsigned>(p.distance_m));
            swp::Place copy = p;
            list_row(results_, p.name.c_str(), p.category.c_str(), dist, [copy] { push(new PlaceScreen(copy)); }, color::red(), LV_SYMBOL_GPS);
        }
    }

private:
    lv_obj_t* results_ = nullptr;
};

void MapScreen::open_search() {
    send_search("");  // immediately show nearby POIs/businesses
    push(new SearchScreen());
}

}  // namespace

Screen* make_maps() { return new MapScreen(); }

}  // namespace ui
