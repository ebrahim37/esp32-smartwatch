// Home watch face: near-black, minimal colour, always-on friendly.
#include <algorithm>
#include <cmath>
#include <vector>

#include "app/services.hpp"
#include "app/settings.hpp"
#include "board.hpp"
#include "app/state.hpp"
#include "core/format.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include "ui/ui.hpp"

namespace ui {
namespace {

lv_color_t level_color(core::Level l) {
    switch (l) {
        case core::Level::Good: return color::green();
        case core::Level::Moderate: return color::yellow();
        case core::Level::High: return color::orange();
        case core::Level::Severe: return color::red();
        default: return color::muted();
    }
}

// Layout: three rows separated by horizontal dividers, with one vertical divider line shared
// by all rows (kSplit) so they line up. Right-column text is right-aligned to kContentR, the
// same edge as the weather row's wind/outlook text (checked against a photo of the watch).
constexpr int kLeft = 24, kRight = 386, kSplit = 254, kContentR = 368;
constexpr const char* kLinkSym = "\xEF\x83\x81";  // FontAwesome link (U+F0C1)
constexpr const char* kWalkSym = "\xEF\x95\x94";  // FontAwesome walking (U+F554)
constexpr const char* kBellOffSym = "\xEF\x87\xB6";  // FontAwesome bell-slash (U+F1F6)

std::string thousands(uint32_t v) {
    std::string s = std::to_string(v);
    for (int i = static_cast<int>(s.size()) - 3; i > 0; i -= 3) s.insert(static_cast<size_t>(i), ",");
    return s;
}

class Home : public Screen {
public:
    bool has_top_bar() const override { return false; }
    bool scrollable() const override { return false; }
    uint32_t tick_ms() const override { return 2000; }

    void build(lv_obj_t* c) override {
        // Everything lives in one container that is nudged a few pixels every minute to
        // spread AMOLED wear (anti burn-in).
        face_ = lv_obj_create(c);
        lv_obj_remove_style_all(face_);
        lv_obj_set_size(face_, 410, 502);
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);

        // Row 1: date + time (opens Calendar) | LINK / NET.
        lv_obj_t* dt = lv_obj_create(face_);
        lv_obj_remove_style_all(dt);
        lv_obj_set_size(dt, kSplit - kLeft - 8, 120);
        lv_obj_set_pos(dt, kLeft, 26);
        lv_obj_add_flag(dt, LV_OBJ_FLAG_CLICKABLE);
        on_click(dt, [] { push(make_calendar()); });
        date_ = label(dt, "", &font_inter_24, color::red());
        lv_obj_set_pos(date_, 16, 4);
        time_ = label(dt, "--:--", &font_digits_84, color::text());
        lv_obj_set_pos(time_, 16, 40);
        auto status = [&](const char* sym, int icon_x, const char* text, int y, lv_obj_t*& icon, lv_obj_t*& txt) {
            icon = label(face_, sym, &font_inter_24, color::muted());
            lv_obj_set_pos(icon, icon_x, y);
            txt = label(face_, text, &font_inter_24);
            lv_obj_align(txt, LV_ALIGN_TOP_RIGHT, kContentR - 410, y);
        };
        status(kLinkSym, kSplit + 20, "LINK", 54, link_icon_, link_txt_);
        status(LV_SYMBOL_WIFI, kSplit + 18, "NET", 98, net_icon_, net_txt_);  // wider glyph: optically aligned
        divs_.push_back(vdiv(40, 102));
        divs_.push_back(hdiv(150));

        // Row 2: weather (opens Weather).
        wx_ = weather_row(face_, kContentR - kLeft, kSplit - kLeft);
        lv_obj_set_pos(wx_, kLeft, 160);
        lv_obj_add_flag(wx_, LV_OBJ_FLAG_CLICKABLE);
        on_click(wx_, [] { push(make_weather()); });
        divs_.push_back(hdiv(226));

        // Row 3: steps (opens Activity) | battery.
        steps_ = lv_obj_create(face_);
        lv_obj_remove_style_all(steps_);
        lv_obj_set_size(steps_, kSplit - kLeft - 8, 50);
        lv_obj_set_pos(steps_, kLeft, 236);
        lv_obj_add_flag(steps_, LV_OBJ_FLAG_CLICKABLE);
        on_click(steps_, [] { push(make_activity()); });
        lv_obj_set_flex_flow(steps_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(steps_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
        lv_obj_set_style_pad_left(steps_, 18, 0);
        lv_obj_set_style_pad_bottom(steps_, 8, 0);
        lv_obj_set_style_pad_column(steps_, 10, 0);
        lv_obj_t* walk = label(steps_, kWalkSym, &font_inter_24);
        lv_obj_set_style_pad_bottom(walk, 4, 0);
        steps_n_ = label(steps_, "0", &font_inter_32);
        lv_obj_t* unit = label(steps_, "steps", &font_inter_24, color::muted());
        lv_obj_set_style_pad_bottom(unit, 3, 0);
        steps_div_ = vdiv(238, 46);
        divs_.push_back(steps_div_);
        // Battery: outline, nub and a fill proportional to the charge.
        bat_box_ = lv_obj_create(face_);
        lv_obj_remove_style_all(bat_box_);
        lv_obj_set_size(bat_box_, 40, 22);
        lv_obj_set_pos(bat_box_, kSplit + 20, 250);
        lv_obj_set_style_border_width(bat_box_, 2, 0);
        lv_obj_set_style_border_color(bat_box_, color::text(), 0);
        lv_obj_set_style_radius(bat_box_, 6, 0);
        lv_obj_set_style_pad_all(bat_box_, 3, 0);
        lv_obj_remove_flag(bat_box_, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        bat_fill_ = lv_obj_create(bat_box_);
        lv_obj_remove_style_all(bat_fill_);
        lv_obj_set_style_bg_opa(bat_fill_, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bat_fill_, 3, 0);
        lv_obj_set_height(bat_fill_, LV_PCT(100));
        lv_obj_t* nub = lv_obj_create(face_);
        lv_obj_remove_style_all(nub);
        lv_obj_set_size(nub, 4, 10);
        lv_obj_set_pos(nub, kSplit + 61, 256);
        lv_obj_set_style_bg_opa(nub, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(nub, color::text(), 0);
        lv_obj_set_style_radius(nub, 2, 0);
        bat_ = label(face_, "--", &font_inter_24);
        lv_obj_align(bat_, LV_ALIGN_TOP_RIGHT, kContentR - 410, 247);
        divs_.push_back(hdiv(296));

        // Bottom shortcuts: Music, Notifications and the app launcher.
        lv_obj_t* music = round_button(face_, LV_SYMBOL_AUDIO, color::pink(), 72, [] { push(make_music()); });
        lv_obj_align(music, LV_ALIGN_BOTTOM_LEFT, 64, -22);
        // Notifications: tap opens the list, hold toggles Do Not Disturb.
        notifs_ = round_button(face_, LV_SYMBOL_BELL, color::purple(), 72, nullptr);
        lv_obj_align(notifs_, LV_ALIGN_BOTTOM_MID, 0, -22);
        on(notifs_, LV_EVENT_SHORT_CLICKED, [] { push(make_notifications()); });
        on(notifs_, LV_EVENT_LONG_PRESSED, [this] {
            settings::set_dnd(!settings::dnd());
            board::vibrate(settings::dnd() ? 200 : 80);
            update_dnd();
        });
        lv_obj_t* notifs = notifs_;
        lv_obj_t* apps = lv_button_create(face_);
        lv_obj_set_size(apps, 72, 72);
        lv_obj_set_style_radius(apps, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(apps, color::card2(), 0);
        lv_obj_set_style_shadow_width(apps, 0, 0);
        lv_obj_align(apps, LV_ALIGN_BOTTOM_RIGHT, -64, -22);
        on_click(apps, [] { push(make_launcher()); });
        // Launchpad-style 3x3 dot grid icon.
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col) {
                lv_obj_t* d = lv_obj_create(apps);
                lv_obj_remove_style_all(d);
                lv_obj_set_size(d, 8, 8);
                lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
                lv_obj_set_style_bg_color(d, color::text(), 0);
                lv_obj_align(d, LV_ALIGN_CENTER, (col - 1) * 13, (r - 1) * 13);
                lv_obj_remove_flag(d, LV_OBJ_FLAG_CLICKABLE);
            }
        buttons_ = {music, notifs, apps};
        update_dnd();
        refresh(0xFFFFFFFFu);
        tick();
    }

    void refresh(uint32_t dirty) override {
        if (dirty & (app::kDirtyTime | app::kDirtySettings)) update_time();
        if (dirty & (app::kDirtyWeather | app::kDirtyTime)) update_weather();
        if (dirty & (app::kDirtyLink | app::kDirtyCompanion)) update_status();
    }

    void tick() override {
        auto p = services::power();
        // TODO(HARDWARE): no battery is fitted on the USB-powered prototype; show "-"
        // ("-") rather than inventing a percentage. Real telemetry appears automatically once a
        // cell is connected to the AXP2101.
        int pct = p.battery_present && p.percent >= 0 ? p.percent : -1;
        set_text(bat_, pct >= 0 ? std::to_string(pct) : "-");
        lv_obj_set_width(bat_fill_, LV_PCT(pct >= 0 ? std::max(pct, 8) : 100));  // no battery (USB): full
        lv_obj_set_style_bg_color(bat_fill_, pct >= 0 ? level_color(core::battery_level(pct)) : color::green(), 0);
        set_text(steps_n_, thousands(services::steps_today()));
        if (++ticks_ % 30 == 0) {  // every 60 s
            lv_obj_set_pos(face_, static_cast<int>(esp_random() % 9) - 4, static_cast<int>(esp_random() % 9) - 4);
        }
        update_time();
        update_status();  // NET goes grey 90 s after the last internet exchange
    }

    // Dimmed (inactive): date, time, weather icon + temperature, precipitation if coming, battery.
    void on_dim(bool dimmed) override {
        auto show = [dimmed](lv_obj_t* o) {
            if (dimmed) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
        };
        for (lv_obj_t* o : divs_) show(o);
        for (lv_obj_t* o : buttons_) show(o);
        for (lv_obj_t* o : {link_icon_, link_txt_, net_icon_, net_txt_, steps_}) show(o);
        weather_row_compact(wx_, dimmed);
    }

private:
    lv_obj_t* vdiv(int y, int h) {
        lv_obj_t* d = vdivider(face_, h);
        lv_obj_set_pos(d, kSplit - 1, y);
        return d;
    }
    lv_obj_t* hdiv(int y) {
        lv_obj_t* d = hdivider(face_, kRight - kLeft);
        lv_obj_set_pos(d, kLeft, y);
        return d;
    }

    void update_dnd() {
        bool on = settings::dnd();
        set_text(lv_obj_get_child(notifs_, 0), on ? kBellOffSym : LV_SYMBOL_BELL);
        lv_obj_set_style_bg_color(notifs_, on ? color::card2() : color::purple(), 0);
    }

    void update_time() {
        struct tm lt;
        if (!services::local_now(lt)) {
            set_text(time_, "--:--");
            set_text(date_, "Time not synced");
            return;
        }
        set_text(time_, core::fmt_clock(lt, false));
        set_text(date_, core::fmt_date_short(lt));
    }

    void update_weather() {
        app::Locked s;
        weather_row_set(wx_, s->weather, s->weather_valid);
    }

    void update_status() {
        bool up, peer, net;
        {
            app::Locked s;
            up = s->link.state == app::LinkState::Ready;
            peer = s->link.has_peer;
            // Last successful internet exchange through the companion, aged to now.
            int64_t age = s->companion_valid && s->companion.internet_age_s != UINT32_MAX
                              ? s->companion.internet_age_s + (esp_timer_get_time() - s->companion_rx_us) / 1000000
                              : INT64_MAX;
            net = up && age < 90;
        }
        lv_obj_set_style_text_color(link_icon_, up ? color::green() : (peer ? color::orange() : color::muted()), 0);
        lv_obj_set_style_text_color(link_txt_, up ? color::text() : color::muted(), 0);
        lv_obj_set_style_text_color(net_icon_, net ? color::green() : color::muted(), 0);
        lv_obj_set_style_text_color(net_txt_, net ? color::text() : color::muted(), 0);
    }

    lv_obj_t *face_ = nullptr, *date_ = nullptr, *time_ = nullptr, *wx_ = nullptr, *notifs_ = nullptr;
    lv_obj_t *link_icon_ = nullptr, *link_txt_ = nullptr, *net_icon_ = nullptr, *net_txt_ = nullptr;
    lv_obj_t *steps_ = nullptr, *steps_n_ = nullptr, *steps_div_ = nullptr;
    lv_obj_t *bat_box_ = nullptr, *bat_fill_ = nullptr, *bat_ = nullptr;
    std::vector<lv_obj_t*> divs_, buttons_;
    uint32_t ticks_ = 0;
};

}  // namespace

Screen* make_home() { return new Home(); }

}  // namespace ui
