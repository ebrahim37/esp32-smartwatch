// Home watch face: near-black, minimal colour, always-on friendly.
#include <cmath>

#include "app/services.hpp"
#include "app/state.hpp"
#include "core/format.hpp"
#include "esp_random.h"
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

class Home : public Screen {
public:
    bool has_top_bar() const override { return false; }
    bool scrollable() const override { return false; }
    uint32_t tick_ms() const override { return 2000; }  // HDG refresh period

    void build(lv_obj_t* c) override {
        // Everything lives in one container that is nudged a few pixels every minute to
        // spread AMOLED wear (anti burn-in).
        face_ = lv_obj_create(c);
        lv_obj_remove_style_all(face_);
        lv_obj_set_size(face_, 410, 502);
        lv_obj_remove_flag(face_, LV_OBJ_FLAG_SCROLLABLE);

        hdg_ = label(face_, "HDG: --", &font_inter_20, color::muted());
        lv_obj_align(hdg_, LV_ALIGN_TOP_LEFT, 58, 28);
        bat_lbl_ = label(face_, "BAT:", &font_inter_20, color::muted());
        lv_obj_align(bat_lbl_, LV_ALIGN_TOP_RIGHT, -118, 28);
        bat_ = label(face_, "--", &font_inter_20, color::muted());
        lv_obj_align_to(bat_, bat_lbl_, LV_ALIGN_OUT_RIGHT_MID, 6, 0);

        // Date + time are one tap target (opens Calendar).
        lv_obj_t* dt = lv_obj_create(face_);
        lv_obj_remove_style_all(dt);
        lv_obj_set_size(dt, 380, 200);
        lv_obj_align(dt, LV_ALIGN_TOP_MID, 0, 58);
        lv_obj_add_flag(dt, LV_OBJ_FLAG_CLICKABLE);
        on_click(dt, [] { push(make_calendar()); });
        date_ = label(dt, "", &font_inter_24, color::red());
        lv_obj_align(date_, LV_ALIGN_TOP_MID, 0, 6);
        time_ = label(dt, "--:--", &font_digits_132, color::text());
        lv_obj_align(time_, LV_ALIGN_TOP_MID, 0, 40);

        // Weather block (opens Weather).
        lv_obj_t* wx = lv_obj_create(face_);
        lv_obj_remove_style_all(wx);
        lv_obj_set_size(wx, 380, 130);
        lv_obj_align(wx, LV_ALIGN_TOP_MID, 0, 262);
        lv_obj_add_flag(wx, LV_OBJ_FLAG_CLICKABLE);
        on_click(wx, [] { push(make_weather()); });
        lv_obj_set_flex_flow(wx, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(wx, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(wx, 6, 0);
        wx_line_ = label(wx, "", &font_inter_24);
        lv_label_set_recolor(wx_line_, true);
        wind_uv_ = label(wx, "", &font_inter_20, color::muted());
        lv_label_set_recolor(wind_uv_, true);
        precip_ = label(wx, "", &font_inter_20, color::teal());

        // Bottom shortcuts: Music, Notifications and the app launcher.
        lv_obj_t* music = round_button(face_, LV_SYMBOL_AUDIO, color::pink(), 72, [] { push(make_music()); });
        lv_obj_align(music, LV_ALIGN_BOTTOM_LEFT, 64, -22);
        lv_obj_t* notifs = round_button(face_, LV_SYMBOL_BELL, color::purple(), 72, [] { push(make_notifications()); });
        lv_obj_align(notifs, LV_ALIGN_BOTTOM_MID, 0, -22);
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
        link_dot_ = lv_obj_create(face_);
        lv_obj_remove_style_all(link_dot_);
        lv_obj_set_size(link_dot_, 10, 10);
        lv_obj_set_style_radius(link_dot_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_opa(link_dot_, LV_OPA_COVER, 0);
        lv_obj_align(link_dot_, LV_ALIGN_TOP_MID, 0, 36);  // companion link, level with HDG / BAT
        refresh(0xFFFFFFFFu);
        tick();
    }

    void refresh(uint32_t dirty) override {
        if (dirty & (app::kDirtyTime | app::kDirtySettings)) update_time();
        if (dirty & (app::kDirtyWeather | app::kDirtyTime)) update_weather();
        if (dirty & app::kDirtyLink) {
            app::Locked s;
            bool up = s->link.state == app::LinkState::Ready;
            lv_obj_set_style_bg_color(link_dot_, up ? color::green() : (s->link.has_peer ? color::orange() : color::card2()), 0);
        }
    }

    void tick() override {
        char b[24];
        snprintf(b, sizeof b, "HDG: %d\xC2\xB0", static_cast<int>(std::lround(services::heading_deg())) % 360);
        set_text(hdg_, b);
        auto p = services::power();
        // TODO(HARDWARE): no battery is fitted on the USB-powered prototype; show "USB"
        // rather than inventing a percentage. Real telemetry appears automatically once a
        // cell is connected to the AXP2101.
        if (p.battery_present && p.percent >= 0) {
            snprintf(b, sizeof b, "%d%%", p.percent);
            set_text(bat_, b);
            lv_obj_set_style_text_color(bat_, level_color(core::battery_level(p.percent)), 0);
        } else {
            set_text(bat_, "USB");
            lv_obj_set_style_text_color(bat_, color::muted(), 0);
        }
        lv_obj_align_to(bat_, bat_lbl_, LV_ALIGN_OUT_RIGHT_MID, 6, 0);
        if (++ticks_ % 30 == 0) {  // every 60 s
            lv_obj_set_pos(face_, static_cast<int>(esp_random() % 9) - 4, static_cast<int>(esp_random() % 9) - 4);
        }
        update_time();
    }

private:
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
        if (!s->weather_valid) {
            set_text(wx_line_, "#8E8E93 weather unavailable#");
            set_text(wind_uv_, "");
            set_text(precip_, "");
            return;
        }
        const auto& w = s->weather;
        std::string desc = w.description.empty() ? core::wmo_description(w.wmo_code) : w.description;
        char b[160];
        snprintf(b, sizeof b, "%s #64D2FF %s#/#FFFFFF %s#/#FF453A %s#", desc.c_str(), core::fmt_temp(w.low_c10).c_str(),
                 core::fmt_temp(w.temp_c10).c_str(), core::fmt_temp(w.high_c10).c_str());
        set_text(wx_line_, b);
        float uv = w.uv10 / 10.f;
        lv_color_t uvc = level_color(core::uv_level(uv));
        snprintf(b, sizeof b, "WIND: %d km/h   UV: #%06lX %d/10#", static_cast<int>(std::lround(w.wind_kmh10 / 10.0)),
                 static_cast<unsigned long>(lv_color_to_u32(uvc) & 0xFFFFFF), static_cast<int>(std::lround(uv)));
        set_text(wind_uv_, b);
        set_text(precip_, core::fmt_precip(w.precip_kind, w.precip_in_min, w.precip_horizon_h));
    }

    lv_obj_t *face_ = nullptr, *hdg_ = nullptr, *bat_lbl_ = nullptr, *bat_ = nullptr, *date_ = nullptr, *time_ = nullptr;
    lv_obj_t *wx_line_ = nullptr, *wind_uv_ = nullptr, *precip_ = nullptr, *link_dot_ = nullptr;
    uint32_t ticks_ = 0;
};

}  // namespace

Screen* make_home() { return new Home(); }

}  // namespace ui
