// Compass: Apple-Watch-style dial that turns under a fixed marker, with the heading and direction
// in the middle and the current coordinates below. Long press the dial: make the current
// direction north.
// TODO(PROTOTYPE): the heading is the same relative gyro heading as Maps (services::heading_deg),
// not magnetic north.
#include <cmath>
#include <cstdio>

#include "app/services.hpp"
#include "app/state.hpp"
#include "board.hpp"
#include "core/map_math.hpp"
#include "ui/ui.hpp"

namespace ui {
namespace {

constexpr int kCx = 205, kCy = 240;  // dial centre
constexpr int kDial = 350;           // dial diameter
constexpr int kLetterR = 104;        // radius of the N/E/S/W letters

const char* direction(int deg) {
    static const char* kNames[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return kNames[((deg + 22) / 45) % 8];
}

std::string coord(double v, char pos, char neg) {
    double a = std::fabs(v);
    int d = static_cast<int>(a);
    int m = static_cast<int>((a - d) * 60);
    char b[24];
    std::snprintf(b, sizeof b, "%d\xC2\xB0%02d'%c", d, m, v >= 0 ? pos : neg);  // U+2032 prime isn't in the fonts
    return b;
}

class CompassScreen : public Screen {
public:
    bool has_top_bar() const override { return false; }
    bool scrollable() const override { return false; }
    uint32_t tick_ms() const override { return 100; }

    void build(lv_obj_t* c) override {
        lv_obj_set_style_pad_all(c, 0, 0);

        // Dial: 5-degree ticks, a number every 30 degrees; rotated so the heading sits at the top.
        static const char* kNums[] = {"0", "30", "60", "90", "120", "150", "180", "210", "240", "270", "300", "330", "", nullptr};
        scale_ = lv_scale_create(c);
        lv_obj_set_size(scale_, kDial, kDial);
        lv_obj_set_pos(scale_, kCx - kDial / 2, kCy - kDial / 2);
        lv_scale_set_mode(scale_, LV_SCALE_MODE_ROUND_INNER);
        lv_scale_set_range(scale_, 0, 360);
        lv_scale_set_angle_range(scale_, 360);
        lv_scale_set_total_tick_count(scale_, 73);
        lv_scale_set_major_tick_every(scale_, 6);
        lv_scale_set_label_show(scale_, true);
        lv_scale_set_text_src(scale_, kNums);
        lv_obj_set_style_arc_width(scale_, 0, LV_PART_MAIN);
        lv_obj_set_style_length(scale_, 22, LV_PART_INDICATOR);
        lv_obj_set_style_line_width(scale_, 4, LV_PART_INDICATOR);
        lv_obj_set_style_line_color(scale_, color::text(), LV_PART_INDICATOR);
        lv_obj_set_style_text_font(scale_, &font_inter_16, LV_PART_INDICATOR);
        lv_obj_set_style_text_color(scale_, color::muted(), LV_PART_INDICATOR);
        lv_obj_set_style_pad_radial(scale_, 6, LV_PART_INDICATOR);
        lv_obj_set_style_length(scale_, 12, LV_PART_ITEMS);
        lv_obj_set_style_line_width(scale_, 2, LV_PART_ITEMS);
        lv_obj_set_style_line_color(scale_, lv_color_hex(0x636366), LV_PART_ITEMS);
        lv_obj_add_flag(scale_, LV_OBJ_FLAG_CLICKABLE);
        on(scale_, LV_EVENT_LONG_PRESSED, [] {
            services::reset_heading();
            board::vibrate(40);
            toast("Current direction set as north");
        });

        static const char* kCardinal[] = {"N", "E", "S", "W"};
        for (int i = 0; i < 4; ++i) letters_[i] = label(c, kCardinal[i], &font_inter_24, i == 0 ? color::red() : color::text());

        // Fixed marker at the top: the direction the watch points.
        marker_ = lv_obj_create(c);
        lv_obj_remove_style_all(marker_);
        lv_obj_set_size(marker_, 6, 30);
        lv_obj_set_style_radius(marker_, 3, 0);
        lv_obj_set_style_bg_color(marker_, color::text(), 0);
        lv_obj_set_style_bg_opa(marker_, LV_OPA_COVER, 0);
        lv_obj_set_pos(marker_, kCx - 3, kCy - kDial / 2 - 14);

        deg_ = label(c, "0\xC2\xB0", &font_digits_64);
        dir_ = label(c, "N", &font_inter_32);
        pos_ = label(c, "", &font_inter_16, color::muted());

        place_nav(round_button(c, LV_SYMBOL_LEFT, lv_color_hex(0x2C2C2E), kNavBtn, [] { pop(); }), false);
        place_nav(round_button(c, LV_SYMBOL_HOME, lv_color_hex(0x2C2C2E), kNavBtn, [] { go_home(); }), true);

        update(true);
        refresh(app::kDirtyPosition);
    }

    void tick() override { update(false); }

    void refresh(uint32_t d) override {
        if (!(d & app::kDirtyPosition)) return;
        bool valid;
        swp::Position p;
        {
            app::Locked s;
            valid = s->position_valid;
            p = s->position;
        }
        std::string t = valid ? coord(core::e7_to_deg(p.lat_e7), 'N', 'S') + "   " + coord(core::e7_to_deg(p.lon_e7), 'E', 'W') : "No location";
        set_text(pos_, t);
        lv_obj_align(pos_, LV_ALIGN_TOP_MID, 0, kCy + kDial / 2 + 18);
    }

private:
    void update(bool force) {
        float h = services::heading_deg();
        h = std::fmod(std::fmod(h, 360.f) + 360.f, 360.f);
        int deg = static_cast<int>(std::lround(h)) % 360;
        if (!force && deg == shown_) return;
        shown_ = deg;
        // Scale rotation: angle of value 0, clockwise from 3 o'clock. The heading goes to 12 o'clock.
        lv_scale_set_rotation(scale_, ((270 - deg) % 360 + 360) % 360);
        for (int i = 0; i < 4; ++i) {
            double a = (i * 90 - deg - 90) * M_PI / 180.0;  // screen angle of this cardinal point
            lv_obj_update_layout(letters_[i]);
            int w = lv_obj_get_width(letters_[i]), hgt = lv_obj_get_height(letters_[i]);
            lv_obj_set_pos(letters_[i], kCx + static_cast<int>(std::lround(kLetterR * std::cos(a))) - w / 2,
                           kCy + static_cast<int>(std::lround(kLetterR * std::sin(a))) - hgt / 2);
        }
        set_text(deg_, std::to_string(deg) + "\xC2\xB0");
        lv_obj_align(deg_, LV_ALIGN_TOP_MID, 8, kCy - 52);  // nudged right: the degree sign is narrow
        set_text(dir_, direction(deg));
        lv_obj_align(dir_, LV_ALIGN_TOP_MID, 0, kCy + 18);
    }

    lv_obj_t *scale_ = nullptr, *marker_ = nullptr, *deg_ = nullptr, *dir_ = nullptr, *pos_ = nullptr;
    lv_obj_t* letters_[4] = {};
    int shown_ = -1;
};

}  // namespace

Screen* make_compass() { return new CompassScreen(); }

}  // namespace ui
