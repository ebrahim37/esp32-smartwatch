// Weather, Timer, Stopwatch, Calculator, Calendar, Activity and Voice Memos.
#include <algorithm>
#include <cmath>
#include <ctime>

#include "app/services.hpp"
#include "app/state.hpp"
#include "ble/link.hpp"
#include "core/calculator.hpp"
#include "core/format.hpp"
#include "esp_timer.h"
#include "ui/ui.hpp"

namespace ui {
namespace {

// ---- Weather ---------------------------------------------------------------------------

// Watch-face weather row geometry (see home.cpp), reused so the two look identical.
constexpr int kWxRowW = 344, kWxRowSplit = 230;

class WeatherScreen : public Screen {
public:
    const char* title() const override { return "Weather"; }
    void build(lv_obj_t* c) override {
        c_ = c;
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_row(c, 8, 0);
        swlink::send(swp::WeatherRequest{});
        refresh(0xFFFFFFFFu);
    }
    void refresh(uint32_t d) override {
        if (!(d & app::kDirtyWeather)) return;
        clear(c_);
        swp::Weather w;
        bool valid;
        {
            app::Locked s;
            w = s->weather;
            valid = s->weather_valid;
        }
        if (!valid) {
            label(c_, "No weather data yet", &font_inter_20, color::muted());
            label(c_, "The companion fetches it from the VPS helper.", &font_inter_16, color::muted());
            return;
        }
        lv_obj_t* wx = weather_row(c_, kWxRowW, kWxRowSplit);  // same row as the watch face
        lv_obj_set_style_translate_x(wx, -10, 0);
        weather_row_set(wx, w, true);
        char b[96];

        if (!w.hours.empty()) {
            section_header(c_, "Hourly");
            lv_obj_t* row = lv_obj_create(c_);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, LV_PCT(100), 130);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_style_pad_column(row, 6, 0);
            lv_obj_set_scroll_dir(row, LV_DIR_HOR);
            lv_obj_set_scrollbar_mode(row, LV_SCROLLBAR_MODE_OFF);
            for (size_t i = 0; i < w.hours.size() && i < 12; ++i) {
                const auto& h = w.hours[i];
                lv_obj_t* cell = card(row);
                lv_obj_set_size(cell, 76, 126);
                lv_obj_set_style_pad_ver(cell, 8, 0);
                lv_obj_set_style_pad_hor(cell, 4, 0);
                lv_obj_set_style_pad_row(cell, 2, 0);
                lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
                lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
                time_t tt = h.time;
                struct tm lt;
                localtime_r(&tt, &lt);
                label(cell, core::fmt_hour12(lt.tm_hour).c_str(), &font_inter_16, color::muted());
                bool night = lt.tm_hour < 6 || lt.tm_hour >= 20;
                label(cell, wx_icon(h.wmo_code, night), &font_inter_24, wx_color(h.wmo_code, night));
                label(cell, (core::fmt_temp(h.temp_c10) + "\xC2\xB0").c_str(), &font_inter_20);
                snprintf(b, sizeof b, "%u%%", h.precip_prob);
                label(cell, b, &font_inter_16, color::teal());
            }
        }
        if (!w.days.empty()) {
            section_header(c_, "7-Day");
            static const char* wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
            time_t now = time(nullptr);
            struct tm today;
            localtime_r(&now, &today);
            for (auto& d2 : w.days) {
                time_t tt = d2.date;  // UTC midnight of the forecast's calendar day
                struct tm lt;
                gmtime_r(&tt, &lt);
                std::string sub = std::to_string(d2.precip_prob) + "% precip.";
                std::string val = core::fmt_temp(d2.low_c10) + "\xC2\xB0 / " + core::fmt_temp(d2.high_c10) + "\xC2\xB0";
                bool is_today = lt.tm_year == today.tm_year && lt.tm_yday == today.tm_yday;
                lv_obj_t* r = list_row(c_, is_today ? "Today" : wd[lt.tm_wday % 7], sub.c_str(), val.c_str(), nullptr, color::card(),
                                       wx_icon(d2.wmo_code, false));
                lv_obj_t* ic = lv_obj_get_child(lv_obj_get_child(r, 0), 0);  // list_row's icon label
                lv_obj_set_style_text_font(ic, &font_inter_24, 0);
                lv_obj_set_style_text_color(ic, wx_color(d2.wmo_code, false), 0);
            }
        }
        if (w.updated_at) {
            time_t tt = w.updated_at;
            struct tm lt;
            localtime_r(&tt, &lt);
            snprintf(b, sizeof b, "Updated %02d:%02d \xC2\xB7 Open-Meteo", lt.tm_hour, lt.tm_min);
            label(c_, b, &font_inter_16, color::muted());
        }
    }

private:
    lv_obj_t* c_ = nullptr;
};

// ---- Timer -----------------------------------------------------------------------------

class TimerScreen : public Screen {
    // Rounding of the remaining time: 999 = round up, 500 = nearest, 0 = round down. Larger
    // values change each digit later. Matched by eye against the iPhone timer.
    static constexpr int64_t kRoundMs = 700;

public:
    const char* title() const override { return "Timer"; }
    bool scrollable() const override { return false; }
    uint32_t tick_ms() const override { return 100; }
    ~TimerScreen() override {
        if (digit_timer_) lv_timer_delete(digit_timer_);
    }
    void build(lv_obj_t* c) override {
        // One-shot timer that lands exactly on the next second change, so the digits do not
        // trail the real countdown by up to a tick period.
        digit_timer_ = lv_timer_create(
            [](lv_timer_t* lt) {
                lv_timer_pause(lt);
                static_cast<TimerScreen*>(lv_timer_get_user_data(lt))->tick();
                lv_refr_now(nullptr);
            },
            1000, this);
        lv_timer_pause(digit_timer_);
        auto t = services::timer();
        if (t.state == services::TimerState::Idle && t.duration_s == 600) services::timer_set(600);  // opens at 10:00
        ring_ = lv_arc_create(c);
        lv_obj_set_size(ring_, 230, 230);
        lv_obj_align(ring_, LV_ALIGN_TOP_MID, 0, 6);
        lv_arc_set_bg_angles(ring_, 0, 360);
        lv_arc_set_rotation(ring_, 270);
        lv_arc_set_range(ring_, 0, 1000);
        lv_obj_set_style_arc_width(ring_, 12, LV_PART_MAIN);
        lv_obj_set_style_arc_width(ring_, 12, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(ring_, color::card2(), LV_PART_MAIN);
        lv_obj_set_style_arc_color(ring_, color::orange(), LV_PART_INDICATOR);
        lv_obj_remove_style(ring_, nullptr, LV_PART_KNOB);
        lv_obj_remove_flag(ring_, LV_OBJ_FLAG_CLICKABLE);
        time_ = label(c, "10:00", &font_digits_64);
        lv_obj_align_to(time_, ring_, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(time_, LV_OBJ_FLAG_CLICKABLE);
        on_click(time_, [] {
            if (services::timer().state == services::TimerState::Running) return;
            KeyboardOpts o;
            o.label = "Minutes";
            o.numeric = true;
            o.max_len = 3;
            open_keyboard(o, [](const std::string& s) {
                int m = atoi(s.c_str());
                if (m > 0) services::timer_set(static_cast<uint32_t>(m) * 60);
            });
        });
        minus_ = round_button(c, LV_SYMBOL_MINUS, color::card(), 64, [] { adjust(-60); });
        lv_obj_align(minus_, LV_ALIGN_TOP_LEFT, 16, 100);
        plus_ = round_button(c, LV_SYMBOL_PLUS, color::card(), 64, [] { adjust(60); });
        lv_obj_align(plus_, LV_ALIGN_TOP_RIGHT, -16, 100);
        reset_ = pill_button(c, "Reset", color::card2(), [] { services::timer_reset(); }, 160, 72);
        lv_obj_align(reset_, LV_ALIGN_BOTTOM_LEFT, 24, -44);
        start_ = pill_button(c, "Start", color::orange(), [] {
            auto t = services::timer();
            if (t.state == services::TimerState::Running) services::timer_pause();
            else services::timer_start();
        }, 160, 72);
        lv_obj_align(start_, LV_ALIGN_BOTTOM_RIGHT, -24, -44);
        lv_obj_t* hint = label(c, "Tap the time to type minutes", &font_inter_16, color::muted());
        lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -12);
        tick();
    }
    void tick() override {
        auto t = services::timer();
        int64_t sec = (t.remaining_ms + kRoundMs) / 1000;
        set_text(time_, core::fmt_duration(sec));
        lv_obj_align_to(time_, ring_, LV_ALIGN_CENTER, 0, 0);
        lv_arc_set_value(ring_, t.duration_s ? static_cast<int32_t>(1000 * t.remaining_ms / (t.duration_s * 1000LL)) : 0);
        bool running = t.state == services::TimerState::Running;
        // The digits change when remaining_ms + kRoundMs crosses a whole second.
        uint32_t to_change = static_cast<uint32_t>((t.remaining_ms + kRoundMs) % 1000);
        if (running && to_change > 0 && to_change <= tick_ms()) {
            lv_timer_set_period(digit_timer_, to_change + 1);
            lv_timer_reset(digit_timer_);
            lv_timer_resume(digit_timer_);
        }
        set_text(lv_obj_get_child(start_, 0), running ? "Pause" : (t.state == services::TimerState::Paused ? "Resume" : "Start"));
        lv_obj_set_style_bg_color(start_, running ? color::card2() : color::orange(), 0);
        if (running) {
            lv_obj_add_flag(minus_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(plus_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(minus_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(plus_, LV_OBJ_FLAG_HIDDEN);
        }
    }

private:
    static void adjust(int delta) {
        auto t = services::timer();
        int64_t d = static_cast<int64_t>(t.duration_s) + delta;
        if (d < 60) d = 60;
        services::timer_set(static_cast<uint32_t>(d));
    }
    lv_obj_t *ring_ = nullptr, *time_ = nullptr, *start_ = nullptr, *reset_ = nullptr, *minus_ = nullptr, *plus_ = nullptr;
    lv_timer_t* digit_timer_ = nullptr;
};

// ---- Stopwatch -------------------------------------------------------------------------

class StopwatchScreen : public Screen {
public:
    const char* title() const override { return "Stopwatch"; }
    bool scrollable() const override { return false; }
    uint32_t tick_ms() const override { return 100; }
    void build(lv_obj_t* c) override {
        time_ = label(c, "00:00.00", &font_digits_64);
        lv_obj_align(time_, LV_ALIGN_TOP_MID, 0, 20);
        laps_ = lv_obj_create(c);
        lv_obj_remove_style_all(laps_);
        lv_obj_set_size(laps_, 340, 200);
        lv_obj_align(laps_, LV_ALIGN_TOP_MID, 0, 96);
        lv_obj_set_flex_flow(laps_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(laps_, 4, 0);
        lap_ = pill_button(c, "Lap", color::card2(), [] {
            if (services::stopwatch().running) services::stopwatch_lap();
            else services::stopwatch_reset();
        }, 160, 72);
        lv_obj_align(lap_, LV_ALIGN_BOTTOM_LEFT, 24, -36);
        start_ = pill_button(c, "Start", color::green(), [] { services::stopwatch_toggle(); }, 160, 72);
        lv_obj_align(start_, LV_ALIGN_BOTTOM_RIGHT, -24, -36);
        tick();
    }
    void tick() override {
        auto s = services::stopwatch();
        set_text(time_, core::fmt_stopwatch(s.elapsed_us));
        set_text(lv_obj_get_child(start_, 0), s.running ? "Stop" : "Start");
        lv_obj_set_style_bg_color(start_, s.running ? color::red() : color::green(), 0);
        set_text(lv_obj_get_child(lap_, 0), s.running || s.elapsed_us == 0 ? "Lap" : "Reset");
        if (s.laps.size() != shown_laps_) {
            shown_laps_ = s.laps.size();
            clear(laps_);
            for (size_t i = s.laps.size(); i-- > 0 && s.laps.size() - i <= 6;) {
                int64_t split = s.laps[i] - (i ? s.laps[i - 1] : 0);
                std::string t = "Lap " + std::to_string(i + 1) + "    " + core::fmt_stopwatch(split);
                label(laps_, t.c_str(), &font_inter_20, color::muted());
            }
        }
    }

private:
    lv_obj_t *time_ = nullptr, *laps_ = nullptr, *lap_ = nullptr, *start_ = nullptr;
    size_t shown_laps_ = SIZE_MAX;
};

// ---- Calculator ------------------------------------------------------------------------

// Clear key text is swapped between "AC" and "C"; the map is re-applied when it changes.
const char* kCalcMap[] = {"AC", "+/-", "%", "\xC3\xB7", "\n", "7", "8", "9", "\xC3\x97", "\n", "4", "5", "6", "-", "\n",
                          "1", "2", "3", "+", "\n", "0", ".", "=", ""};
// "0" spans two columns.
const lv_buttonmatrix_ctrl_t kCalcCtrl[] = {
    (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1,
    (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1,
    (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1,
    (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1,
    (lv_buttonmatrix_ctrl_t)2, (lv_buttonmatrix_ctrl_t)1, (lv_buttonmatrix_ctrl_t)1};
constexpr uint32_t kEqualsId = 18;

class CalculatorScreen : public Screen {
public:
    const char* title() const override { return "Calculator"; }
    bool scrollable() const override { return false; }
    void build(lv_obj_t* c) override {
        // Keypad sized so its corner keys stay inside the panel's rounded corners.
        constexpr int kPadW = 340, kPadH = 300;
        disp_ = label(c, "0", &font_digits_64);
        lv_obj_set_width(disp_, kPadW - 64);
        lv_obj_set_style_text_align(disp_, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(disp_, LV_LABEL_LONG_CLIP);
        lv_obj_align(disp_, LV_ALIGN_TOP_LEFT, (410 - kPadW) / 2, 8);
        // Backspace sits right of the number: grey glyph, no background.
        lv_obj_t* bsp = round_button(c, LV_SYMBOL_BACKSPACE, color::bg(), 56, [this] {
            calc_.backspace();
            update();
        });
        lv_obj_set_style_bg_opa(bsp, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_opa(bsp, LV_OPA_20, LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(bsp, color::muted(), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(lv_obj_get_child(bsp, 0), color::muted(), 0);
        lv_obj_align(bsp, LV_ALIGN_TOP_RIGHT, -(410 - kPadW) / 2 + 4, 16);

        m_ = lv_buttonmatrix_create(c);
        lv_obj_set_size(m_, kPadW, kPadH);
        lv_obj_align(m_, LV_ALIGN_BOTTOM_MID, 0, -24);
        lv_obj_set_style_bg_opa(m_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(m_, 0, 0);
        lv_obj_set_style_pad_all(m_, 2, 0);
        lv_obj_set_style_pad_gap(m_, 8, 0);
        lv_obj_set_style_radius(m_, LV_RADIUS_CIRCLE, LV_PART_ITEMS);
        lv_obj_set_style_bg_color(m_, color::card2(), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(m_, color::muted(), sel(LV_PART_ITEMS, LV_STATE_PRESSED));
        lv_obj_set_style_text_font(m_, &font_inter_32, LV_PART_ITEMS);
        lv_obj_set_style_text_color(m_, color::text(), LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(m_, 0, LV_PART_ITEMS);
        apply_map();
        lv_obj_add_event_cb(m_, on_key, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(m_, on_draw, LV_EVENT_DRAW_TASK_ADDED, this);
        lv_obj_add_flag(m_, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }

private:
    void apply_map() {
        kCalcMap[0] = calc_.has_entry() ? "C" : "AC";
        lv_buttonmatrix_set_map(m_, kCalcMap);
        lv_buttonmatrix_set_ctrl_map(m_, kCalcCtrl);
    }
    static bool is_operator(uint32_t id) { return id == kEqualsId || (id < 16 && id % 4 == 3); }
    static void on_draw(lv_event_t* e) {
        // Operator column in orange, top row in light grey (calculator convention). Pressed keys
        // get a lighter shade, since these custom fills override LVGL's pressed style.
        lv_draw_task_t* t = lv_event_get_draw_task(e);
        lv_draw_dsc_base_t* base = static_cast<lv_draw_dsc_base_t*>(lv_draw_task_get_draw_dsc(t));
        if (base->part != LV_PART_ITEMS || lv_draw_task_get_type(t) != LV_DRAW_TASK_TYPE_FILL) return;
        uint32_t id = base->id1;
        auto* fill = static_cast<lv_draw_fill_dsc_t*>(lv_draw_task_get_draw_dsc(t));
        lv_obj_t* m = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
        bool pressed = lv_obj_has_state(m, LV_STATE_PRESSED) && lv_buttonmatrix_get_selected_button(m) == id;
        if (is_operator(id)) fill->color = pressed ? lv_color_hex(0xFFD08A) : color::orange();
        else if (id < 3) fill->color = pressed ? lv_color_hex(0xA1A1A6) : lv_color_hex(0x636366);
    }
    static void on_key(lv_event_t* e) {
        auto* self = static_cast<CalculatorScreen*>(lv_event_get_user_data(e));
        lv_obj_t* m = static_cast<lv_obj_t*>(lv_event_get_target(e));
        uint32_t id = lv_buttonmatrix_get_selected_button(m);
        const char* t = lv_buttonmatrix_get_button_text(m, id);
        if (!t) return;
        std::string k = t;
        auto& c = self->calc_;
        using Op = core::Calculator::Op;
        if (k.size() == 1 && k[0] >= '0' && k[0] <= '9') c.digit(k[0] - '0');
        else if (k == ".") c.decimal_point();
        else if (k == "AC") c.clear();
        else if (k == "C") c.clear_entry();
        else if (k == "+/-") c.toggle_sign();
        else if (k == "%") c.percent();
        else if (k == "+") c.op(Op::Add);
        else if (k == "-") c.op(Op::Sub);
        else if (k == "\xC3\x97") c.op(Op::Mul);
        else if (k == "\xC3\xB7") c.op(Op::Div);
        else if (k == "=") c.equals();
        self->update();
    }
    void update() {
        std::string d = calc_.display();
        set_text(disp_, d);
        // The digit font only has 0-9 . - : fall back for "Error", exponents and long values.
        bool digits_only = d.find_first_not_of("0123456789.-") == std::string::npos;
        lv_obj_set_style_text_font(disp_, d.size() > 7 || !digits_only ? &font_inter_32 : &font_digits_64, 0);
        // Re-applying the map resets the pressed key, so only do it when the label changes.
        if ((calc_.has_entry() ? "C" : "AC") != std::string(kCalcMap[0])) apply_map();
    }
    core::Calculator calc_;
    lv_obj_t* disp_ = nullptr;
    lv_obj_t* m_ = nullptr;
};

// ---- Calendar --------------------------------------------------------------------------

class CalendarScreen : public Screen {
public:
    const char* title() const override { return "Calendar"; }
    bool scrollable() const override { return false; }
    void build(lv_obj_t* c) override {
        lv_obj_t* cal = lv_calendar_create(c);
        lv_obj_set_size(cal, 360, 360);
        lv_obj_align(cal, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_set_style_bg_color(cal, color::bg(), 0);
        lv_obj_set_style_border_width(cal, 0, 0);
        lv_obj_set_style_text_font(cal, &font_inter_20, 0);
        lv_obj_t* bm = lv_calendar_get_btnmatrix(cal);
        lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(bm, 0, 0);
        lv_obj_set_style_text_color(bm, color::text(), LV_PART_ITEMS);
        lv_calendar_header_arrow_create(cal);
        struct tm lt;
        bool valid = services::local_now(lt);
        if (valid) {
            lv_calendar_set_today_date(cal, lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
            lv_calendar_set_month_shown(cal, lt.tm_year + 1900, lt.tm_mon + 1);
        } else {
            lv_calendar_set_month_shown(cal, 2026, 1);
            lv_obj_t* l = label(c, "Time not synced yet", &font_inter_16, color::muted());
            lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -8);
        }
        lv_obj_set_style_bg_color(bm, color::red(), sel(LV_PART_ITEMS, LV_STATE_CHECKED));
    }
};

// ---- Activity (steps) ------------------------------------------------------------------

class ActivityScreen : public Screen {
public:
    const char* title() const override { return "Activity"; }
    bool scrollable() const override { return false; }
    void build(lv_obj_t* c) override {
        steps_ = label(c, "0", &font_digits_64, color::green());
        lv_obj_align(steps_, LV_ALIGN_TOP_MID, 0, 60);
        lv_obj_t* l = label(c, "steps today", &font_inter_24, color::muted());
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 150);
        lv_obj_t* n = label(c, "Counted from the accelerometer; the total resets at local midnight.", &font_inter_16, color::muted());
        lv_obj_set_width(n, 340);
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(n, LV_ALIGN_TOP_MID, 0, 210);
        tick();
    }
    void tick() override { set_text(steps_, std::to_string(services::steps_today())); }

private:
    lv_obj_t* steps_ = nullptr;
};

// ---- Voice memos ------------------------------------------------------------------------
// Recording runs on the companion through the headphones' microphone (Bluetooth hands-free);
// memos live on its SD card and play back through the music player (playlist "memos").

class MemosScreen : public Screen {
public:
    const char* title() const override { return "Voice Memos"; }
    uint32_t tick_ms() const override { return 250; }
    void build(lv_obj_t* c) override {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_row(c, 10, 0);
        btn_ = round_button(c, UI_SYMBOL_MIC, color::red(), 96, [this] { toggle(); });
        btn_icon_ = lv_obj_get_child(btn_, 0);
        status_ = label(c, "", &font_inter_20, color::muted());
        lv_obj_set_width(status_, 330);
        lv_label_set_long_mode(status_, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(status_, LV_TEXT_ALIGN_CENTER, 0);
        list_ = lv_obj_create(c);
        lv_obj_remove_style_all(list_);
        lv_obj_set_size(list_, LV_PCT(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(list_, 8, 0);
        lv_obj_remove_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(list_, LV_OBJ_FLAG_EVENT_BUBBLE);
        swlink::memo(swp::MemoOp::List);
        refresh(0xFFFFFFFFu);
    }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyMemos | app::kDirtyLink | app::kDirtyPlayback)) show();
    }
    void tick() override { show_status(); }

private:
    void toggle() {
        auto st = state();
        swlink::memo(st == swp::MemoState::Connecting || st == swp::MemoState::Recording ? swp::MemoOp::Stop : swp::MemoOp::Start);
    }
    swp::MemoState state() {
        app::Locked s;
        return static_cast<swp::MemoState>(s->memo.state);
    }

    void show_status() {
        bool ready = swlink::ready();
        swp::MemoStatus m;
        int64_t rx;
        {
            app::Locked s;
            m = s->memo;
            rx = s->memo_rx_us;
        }
        auto st = static_cast<swp::MemoState>(m.state);
        bool busy = ready && (st == swp::MemoState::Connecting || st == swp::MemoState::Recording);
        set_text(btn_icon_, busy ? LV_SYMBOL_STOP : UI_SYMBOL_MIC);
        if (ready) lv_obj_remove_state(btn_, LV_STATE_DISABLED);
        else lv_obj_add_state(btn_, LV_STATE_DISABLED);
        lv_color_t col = color::muted();
        std::string txt;
        if (!ready) {
            txt = "Companion not connected";
        } else if (st == swp::MemoState::Connecting) {
            txt = "Connecting to headphones\xE2\x80\xA6";  // U+2026 ellipsis
        } else if (st == swp::MemoState::Recording) {
            int64_t ms = m.elapsed_ms + (esp_timer_get_time() - rx) / 1000;
            txt = "Recording  " + core::fmt_duration(ms / 1000);
            col = color::red();
        } else if (st == swp::MemoState::Error) {
            txt = m.error.empty() ? "Recording failed" : m.error;
            col = color::orange();
        } else {
            txt = "Tap to record with your headphones";
        }
        set_text(status_, txt);
        lv_obj_set_style_text_color(status_, col, 0);
    }

    void show() {
        show_status();
        std::vector<swp::MemoInfo> memos;
        bool loaded;
        std::string cur;
        {
            app::Locked s;
            memos = s->memos;
            loaded = s->memos_loaded;
            if (s->playback.playlist_id == "memos") cur = s->playback.track_id;
        }
        std::string key = std::to_string(loaded) + cur;
        for (auto& m : memos) key += m.name + "/";
        if (key == shown_) return;
        shown_ = key;
        clear(list_);
        if (!loaded) return;
        if (memos.empty()) {
            section_header(list_, "No memos yet.");
            return;
        }
        for (size_t i = 0; i < memos.size(); ++i) {
            const auto& m = memos[i];
            char when[32];
            time_t t = m.created;
            struct tm lt;
            localtime_r(&t, &lt);
            strftime(when, sizeof when, "%b %e, %l:%M %p", &lt);
            std::string title = when;
            for (size_t p; (p = title.find("  ")) != std::string::npos;) title.erase(p, 1);  // %e / %l pad with spaces
            std::string dur = core::fmt_duration(std::max<uint32_t>(1, m.duration_ms / 1000));
            uint16_t idx = static_cast<uint16_t>(i);
            std::string name = m.name;
            lv_obj_t* r = list_row(list_, title.c_str(), nullptr, dur.c_str(), nullptr, color::card2(), LV_SYMBOL_PLAY);
            lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_bg_color(r, color::card2(), LV_STATE_PRESSED);
            on(r, LV_EVENT_SHORT_CLICKED, [idx] {
                swlink::playback(swp::PlaybackCmd::SetShuffle, 0);
                swlink::playback(swp::PlaybackCmd::SetRepeat, static_cast<uint32_t>(swp::RepeatMode::Off));
                swlink::playback(swp::PlaybackCmd::PlayPlaylist, idx, "memos");
                push(make_now_playing());
            });
            on(r, LV_EVENT_LONG_PRESSED, [name, title] {
                confirm("Delete memo?", title.c_str(), "Delete", color::red(), [name] { swlink::memo(swp::MemoOp::Delete, name); });
            });
            if (m.name == cur) lv_obj_set_style_text_color(r, color::pink(), 0);
        }
        section_header(list_, "Hold a memo to delete it.");
    }

    lv_obj_t *btn_ = nullptr, *btn_icon_ = nullptr, *status_ = nullptr, *list_ = nullptr;
    std::string shown_ = "-";
};

}  // namespace

Screen* make_weather() { return new WeatherScreen(); }
Screen* make_timer() { return new TimerScreen(); }
Screen* make_stopwatch() { return new StopwatchScreen(); }
Screen* make_calculator() { return new CalculatorScreen(); }
Screen* make_calendar() { return new CalendarScreen(); }
Screen* make_activity() { return new ActivityScreen(); }
Screen* make_voice_memos() { return new MemosScreen(); }

}  // namespace ui
