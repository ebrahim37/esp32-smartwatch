#include "ui/ui.hpp"

#include <vector>

#include "app/services.hpp"
#include "app/settings.hpp"
#include "app/state.hpp"
#include "ble/central.hpp"
#include "ble/peripheral.hpp"
#include "board.hpp"
#include "esp_log.h"

namespace ui {
namespace {

constexpr const char* TAG = "ui";
std::vector<Screen*> stack;
lv_timer_t* ui_timer = nullptr;
bool dimmed = false;
lv_obj_t* toast_obj = nullptr;
lv_timer_t* toast_timer = nullptr;
lv_obj_t* alert_obj = nullptr;
lv_obj_t* banner_obj = nullptr;
lv_obj_t* passkey_obj = nullptr;
app::PasskeyMode shown_passkey = app::PasskeyMode::None;

void style_screen(lv_obj_t* o) {
    lv_obj_set_style_bg_color(o, color::bg(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(o, color::text(), 0);
    lv_obj_set_style_text_font(o, &font_inter_20, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
}

void build_top_bar(Screen* s) {
    lv_obj_t* bar = lv_obj_create(s->scr);
    style_screen(bar);
    lv_obj_set_size(bar, LV_PCT(100), kTopBarH);
    lv_obj_align(bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    place_nav(round_button(bar, LV_SYMBOL_LEFT, color::card(), kNavBtn, [] { pop(); }), false);
    place_nav(round_button(bar, LV_SYMBOL_HOME, color::card(), kNavBtn, [] { go_home(); }), true);
    lv_obj_t* t = label(bar, s->title(), &font_inter_24, color::text());
    lv_obj_set_width(t, kTitleW);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, kNavY - 14);
}

void create_screen(Screen* s) {
    s->scr = lv_obj_create(nullptr);
    style_screen(s->scr);
    lv_obj_remove_flag(s->scr, LV_OBJ_FLAG_SCROLLABLE);
    // The C++ screen object lives exactly as long as its LVGL screen.
    lv_obj_add_event_cb(
        s->scr, [](lv_event_t* e) { delete static_cast<Screen*>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, s);
    int top = s->has_top_bar() ? kTopBarH : 0;
    s->content = lv_obj_create(s->scr);
    style_screen(s->content);
    lv_obj_set_size(s->content, LV_PCT(100), board::display_height() - top);
    lv_obj_align(s->content, LV_ALIGN_TOP_MID, 0, top);
    if (!s->scrollable()) {
        lv_obj_remove_flag(s->content, LV_OBJ_FLAG_SCROLLABLE);
    } else {
        lv_obj_set_scroll_dir(s->content, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(s->content, LV_SCROLLBAR_MODE_OFF);
    }
    if (s->has_top_bar()) build_top_bar(s);
    s->build(s->content);
    // Keep the end of scrolled content clear of the rounded bottom corners.
    if (s->scrollable() && lv_obj_get_style_pad_bottom(s->content, LV_PART_MAIN) < kSafeBottom)
        lv_obj_set_style_pad_bottom(s->content, kSafeBottom, 0);
}

void close_overlay(lv_obj_t*& o) {
    if (o) {
        lv_obj_delete(o);
        o = nullptr;
    }
}

lv_obj_t* modal_base() {
    lv_obj_t* m = lv_obj_create(lv_layer_top());
    style_screen(m);
    lv_obj_set_size(m, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_opa(m, LV_OPA_90, 0);
    lv_obj_remove_flag(m, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m, LV_OBJ_FLAG_CLICKABLE);  // swallow touches to the screen below
    return m;
}

void show_next_alert() {
    if (alert_obj) return;
    app::Alert a;
    {
        app::Locked s;
        if (s->alerts.empty()) return;
        a = s->alerts.front();
        s->alerts.erase(s->alerts.begin());
    }
    if (a.kind == app::Alert::Notification) {
        close_overlay(banner_obj);
        banner_obj = lv_obj_create(lv_layer_top());
        lv_obj_set_size(banner_obj, 330, LV_SIZE_CONTENT);
        lv_obj_align(banner_obj, LV_ALIGN_TOP_MID, 0, 26);
        lv_obj_set_style_bg_color(banner_obj, color::card2(), 0);
        lv_obj_set_style_radius(banner_obj, 24, 0);
        lv_obj_set_style_border_width(banner_obj, 0, 0);
        lv_obj_set_style_pad_all(banner_obj, 16, 0);
        lv_obj_set_flex_flow(banner_obj, LV_FLEX_FLOW_COLUMN);
        lv_obj_remove_flag(banner_obj, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t* t = label(banner_obj, a.title.c_str(), &font_inter_20, color::text());
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_set_width(t, LV_PCT(100));
        lv_obj_t* b = label(banner_obj, a.body.c_str(), &font_inter_16, color::muted());
        lv_obj_set_width(b, LV_PCT(100));
        lv_label_set_long_mode(b, LV_LABEL_LONG_DOT);
        lv_obj_set_height(b, 44);
        on_click(banner_obj, [] {
            close_overlay(banner_obj);
            push(make_notifications());
        });
        lv_timer_t* t2 = lv_timer_create([](lv_timer_t*) { close_overlay(banner_obj); }, 5000, nullptr);
        lv_timer_set_repeat_count(t2, 1);
        return;
    }
    alert_obj = modal_base();
    lv_obj_t* icon = label(alert_obj, a.kind == app::Alert::Alarm ? LV_SYMBOL_BELL : LV_SYMBOL_BELL, &font_inter_32,
                           a.kind == app::Alert::Alarm ? color::orange() : color::green());
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 90);
    lv_obj_t* t = label(alert_obj, a.title.c_str(), &font_inter_32);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 140);
    lv_obj_t* b = label(alert_obj, a.body.c_str(), &font_inter_24, color::muted());
    lv_obj_set_width(b, 360);
    lv_obj_set_style_text_align(b, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(b, LV_ALIGN_TOP_MID, 0, 190);
    uint8_t id = a.alarm_id;
    bool alarm = a.kind == app::Alert::Alarm;
    lv_obj_t* d = pill_button(alert_obj, "Dismiss", alarm ? color::orange() : color::green(), [] {
        close_overlay(alert_obj);
        if (services::timer().state == services::TimerState::Done) services::timer_reset();
        show_next_alert();
    }, 300, 64);
    lv_obj_align(d, LV_ALIGN_BOTTOM_MID, 0, alarm ? -120 : -60);
    if (alarm) {
        lv_obj_t* sn = pill_button(alert_obj, "Snooze 9 min", color::card2(), [id] {
            services::alarm_snooze(id, 9);
            close_overlay(alert_obj);
            show_next_alert();
        }, 300, 64);
        lv_obj_align(sn, LV_ALIGN_BOTTOM_MID, 0, -40);
    }
}

void update_passkey_overlay() {
    app::PasskeyMode mode;
    uint32_t value;
    {
        app::Locked s;
        mode = s->passkey_mode;
        value = s->passkey_value;
    }
    if (mode == shown_passkey) return;
    shown_passkey = mode;
    close_overlay(passkey_obj);
    if (mode == app::PasskeyMode::EnterForCompanion) {
        KeyboardOpts o;
        o.label = "Companion passkey";
        o.numeric = true;
        o.max_len = 6;
        open_keyboard(o, [](const std::string& s) { central::submit_passkey(static_cast<uint32_t>(strtoul(s.c_str(), nullptr, 10))); },
                      [] { central::cancel_passkey(); });
        return;
    }
    if (mode == app::PasskeyMode::DisplayForWeb || mode == app::PasskeyMode::ConfirmForWeb) {
        passkey_obj = modal_base();
        lv_obj_t* t = label(passkey_obj, "Pairing request", &font_inter_24, color::muted());
        lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 80);
        char buf[16];
        snprintf(buf, sizeof buf, "%06lu", static_cast<unsigned long>(value));
        lv_obj_t* code = label(passkey_obj, buf, &font_digits_64, color::text());
        lv_obj_align(code, LV_ALIGN_TOP_MID, 0, 150);
        lv_obj_t* hint = label(passkey_obj, mode == app::PasskeyMode::DisplayForWeb ? "Enter this code on the other device" : "Does the other device show this code?",
                               &font_inter_20, color::muted());
        lv_obj_set_width(hint, 340);
        lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 250);
        if (mode == app::PasskeyMode::ConfirmForWeb) {
            lv_obj_t* y = pill_button(passkey_obj, "Accept", color::green(), [] { peripheral::confirm_numeric(true); }, 170, 60);
            lv_obj_align(y, LV_ALIGN_BOTTOM_RIGHT, -24, -50);
            lv_obj_t* n = pill_button(passkey_obj, "Reject", color::card2(), [] { peripheral::confirm_numeric(false); }, 170, 60);
            lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 24, -50);
        }
    } else if (current() && current()->has_top_bar() == false && mode == app::PasskeyMode::None && stack.size() > 1) {
        // Passkey keyboard is closed by the user; nothing else to do.
    }
}

void update_brightness() {
    auto st = settings::get();
    uint32_t idle_ms = lv_display_get_inactive_time(nullptr);
    bool want_dim = idle_ms > st.dim_after_s * 1000u;
    if (want_dim != dimmed) {
        dimmed = want_dim;
        board::set_brightness(want_dim ? st.dim_brightness : st.brightness);
    }
}

void ui_timer_cb(lv_timer_t*) {
    uint32_t dirty = app::take_dirty();
    if (dirty & app::kDirtyAlert) show_next_alert();
    if (dirty & app::kDirtyPasskey) update_passkey_overlay();
    if (dirty & (app::kDirtyAlert | app::kDirtyPasskey)) lv_display_trigger_activity(nullptr);  // wake the screen
    if (dirty & app::kDirtySettings) dimmed = !dimmed;  // force brightness re-apply
    Screen* s = current();
    if (s) {
        if (dirty) s->refresh(dirty);
        s->tick_accum += 100;
        if (s->tick_accum >= s->tick_ms()) {
            s->tick_accum = 0;
            s->tick();
        }
    }
    update_brightness();
}

}  // namespace

void on(lv_obj_t* obj, lv_event_code_t code, Fn fn) {
    auto* f = new Fn(std::move(fn));
    lv_obj_add_event_cb(
        obj, [](lv_event_t* e) { (*static_cast<Fn*>(lv_event_get_user_data(e)))(); }, code, f);
    lv_obj_add_event_cb(
        obj, [](lv_event_t* e) { delete static_cast<Fn*>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, f);
}

void init() {
    lv_display_t* d = board::display();
    lv_theme_t* th = lv_theme_default_init(d, color::blue(), color::green(), true, &font_inter_20);
    lv_display_set_theme(d, th);
    Screen* home = make_home();
    create_screen(home);
    stack.push_back(home);
    lv_screen_load(home->scr);
    ui_timer = lv_timer_create(ui_timer_cb, 100, nullptr);
    board::set_brightness(settings::get().brightness);
    ESP_LOGI(TAG, "UI ready");
}

void push(Screen* s) {
    create_screen(s);
    stack.push_back(s);
    lv_screen_load_anim(s->scr, LV_SCR_LOAD_ANIM_MOVE_LEFT, 160, 0, false);
    s->on_shown();
}

void pop() {
    if (stack.size() <= 1) return;
    stack.pop_back();  // object freed when its LVGL screen is deleted after the animation
    Screen* prev = stack.back();
    lv_screen_load_anim(prev->scr, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 160, 0, true);
    prev->tick_accum = prev->tick_ms();
    prev->refresh(0xFFFFFFFFu);
    prev->on_shown();
}

void pop_n(int n) {
    // Intermediate screens are deleted without animation; one slide back to the target.
    while (n > 1 && stack.size() > 2) {
        lv_obj_delete_async(stack[stack.size() - 2]->scr);
        stack.erase(stack.end() - 2);
        --n;
    }
    pop();
}

void go_home() { pop_n(static_cast<int>(stack.size())); }

Screen* current() { return stack.empty() ? nullptr : stack.back(); }

void place_nav(lv_obj_t* btn, bool right) {
    lv_obj_align(btn, right ? LV_ALIGN_TOP_RIGHT : LV_ALIGN_TOP_LEFT, right ? -(kNavX - kNavBtn / 2) : kNavX - kNavBtn / 2, kNavY - kNavBtn / 2);
}

lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font, lv_color_t c) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, text ? text : "");
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

void set_text(lv_obj_t* lbl, const std::string& s) {
    if (!lbl) return;
    const char* cur = lv_label_get_text(lbl);
    if (cur && s == cur) return;
    lv_label_set_text(lbl, s.c_str());
}

lv_obj_t* pill_button(lv_obj_t* parent, const char* text, lv_color_t bg, Fn fn, int w, int h) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_t* l = label(b, text, h >= 72 ? &font_inter_24 : &font_inter_20, color::text());
    lv_obj_center(l);
    if (fn) on_click(b, std::move(fn));
    return b;
}

lv_obj_t* round_button(lv_obj_t* parent, const char* symbol, lv_color_t bg, int size, Fn fn) {
    lv_obj_t* b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_t* l = label(b, symbol, size >= 72 ? &font_inter_32 : &font_inter_24, color::text());
    lv_obj_center(l);
    if (fn) on_click(b, std::move(fn));
    // Larger touch target than the visual for small buttons.
    lv_obj_set_ext_click_area(b, 8);
    return b;
}

lv_obj_t* card(lv_obj_t* parent) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_style_bg_color(c, color::card(), 0);
    lv_obj_set_style_border_width(c, 0, 0);
    lv_obj_set_style_radius(c, 22, 0);
    lv_obj_set_style_pad_all(c, 14, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t* list_row(lv_obj_t* parent, const char* title, const char* subtitle, const char* value, Fn fn, lv_color_t icon_bg, const char* icon) {
    lv_obj_t* r = card(parent);
    lv_obj_set_width(r, LV_PCT(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 72, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    if (icon) {
        lv_obj_t* ic = lv_obj_create(r);
        lv_obj_set_size(ic, 40, 40);
        lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(ic, icon_bg, 0);
        lv_obj_set_style_border_width(ic, 0, 0);
        lv_obj_remove_flag(ic, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_t* il = label(ic, icon, &font_inter_20);
        lv_obj_center(il);
    }
    lv_obj_t* col = lv_obj_create(r);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_grow(col, 1);
    lv_obj_set_height(col, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(col, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_t* t = label(col, title, &font_inter_20);
    lv_obj_set_width(t, LV_PCT(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    if (subtitle && *subtitle) {
        lv_obj_t* s = label(col, subtitle, &font_inter_16, color::muted());
        lv_obj_set_width(s, LV_PCT(100));
        lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    }
    if (value) label(r, value, &font_inter_16, color::muted());
    if (fn) {
        lv_obj_add_flag(r, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_bg_color(r, color::card2(), LV_STATE_PRESSED);
        on_click(r, std::move(fn));
    }
    return r;
}

lv_obj_t* switch_row(lv_obj_t* parent, const char* title, bool on_state, std::function<void(bool)> fn) {
    lv_obj_t* r = list_row(parent, title);
    lv_obj_t* sw = lv_switch_create(r);
    lv_obj_set_size(sw, 64, 36);
    lv_obj_set_style_bg_color(sw, color::green(), sel(LV_PART_INDICATOR, LV_STATE_CHECKED));
    if (on_state) lv_obj_add_state(sw, LV_STATE_CHECKED);
    auto* f = new std::function<void(bool)>(std::move(fn));
    lv_obj_add_event_cb(
        sw,
        [](lv_event_t* e) {
            auto* fp = static_cast<std::function<void(bool)>*>(lv_event_get_user_data(e));
            (*fp)(lv_obj_has_state(static_cast<lv_obj_t*>(lv_event_get_target(e)), LV_STATE_CHECKED));
        },
        LV_EVENT_VALUE_CHANGED, f);
    lv_obj_add_event_cb(
        sw, [](lv_event_t* e) { delete static_cast<std::function<void(bool)>*>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, f);
    return r;
}

lv_obj_t* section_header(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = label(parent, text, &font_inter_16, color::muted());
    // Full width and wrapping, so longer hints don't run off the screen.
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_pad_top(l, 10, 0);
    lv_obj_set_style_pad_left(l, 8, 0);
    return l;
}

void clear(lv_obj_t* obj) { lv_obj_clean(obj); }

void toast(const std::string& text) {
    close_overlay(toast_obj);
    toast_obj = lv_obj_create(lv_layer_top());
    lv_obj_set_size(toast_obj, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_max_width(toast_obj, 320, 0);
    lv_obj_set_style_bg_color(toast_obj, color::card2(), 0);
    lv_obj_set_style_radius(toast_obj, 20, 0);
    lv_obj_set_style_border_width(toast_obj, 0, 0);
    lv_obj_set_style_pad_all(toast_obj, 14, 0);
    lv_obj_t* l = label(toast_obj, text.c_str(), &font_inter_16);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_max_width(l, 290, 0);
    lv_obj_align(toast_obj, LV_ALIGN_BOTTOM_MID, 0, -36);
    if (toast_timer) lv_timer_delete(toast_timer);
    toast_timer = lv_timer_create(
        [](lv_timer_t*) {
            close_overlay(toast_obj);
            toast_timer = nullptr;
        },
        2500, nullptr);
    lv_timer_set_repeat_count(toast_timer, 1);
}

}  // namespace ui
