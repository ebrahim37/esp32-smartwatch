// App launcher: grid of round app icons.
#include "ui/ui.hpp"

namespace ui {
namespace {

struct AppEntry {
    const char* name;
    const char* icon;
    uint32_t color;
    Screen* (*make)();
};

const AppEntry kApps[] = {
    {"Music", LV_SYMBOL_AUDIO, 0xFF375F, make_music},
    {"Maps", LV_SYMBOL_GPS, 0x30D158, make_maps},
    {"Weather", LV_SYMBOL_IMAGE, 0x0A84FF, make_weather},
    {"Timer", LV_SYMBOL_LOOP, 0xFF9F0A, make_timer},
    {"Stopwatch", LV_SYMBOL_PLAY, 0xFF9F0A, make_stopwatch},
    {"Alarm", LV_SYMBOL_BELL, 0xFF9F0A, make_alarms},
    {"Calculator", LV_SYMBOL_PLUS, 0x8E8E93, make_calculator},
    {"Calendar", LV_SYMBOL_LIST, 0xFF453A, make_calendar},
    {"Alerts", LV_SYMBOL_BELL, 0xBF5AF2, make_notifications},
    {"Activity", LV_SYMBOL_CHARGE, 0x30D158, make_activity},
    {"Memos", LV_SYMBOL_EDIT, 0x636366, make_voice_memos},
    {"Settings", LV_SYMBOL_SETTINGS, 0x636366, make_settings},
};

class Launcher : public Screen {
public:
    const char* title() const override { return "Apps"; }
    void build(lv_obj_t* c) override {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW_WRAP);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_all(c, 10, 0);
        lv_obj_set_style_pad_row(c, 14, 0);
        for (const auto& a : kApps) {
            lv_obj_t* cell = lv_obj_create(c);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, 120, 120);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* ic = lv_obj_create(cell);
            lv_obj_set_size(ic, 84, 84);
            lv_obj_set_style_radius(ic, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(ic, lv_color_hex(a.color), 0);
            lv_obj_set_style_border_width(ic, 0, 0);
            lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 0);
            lv_obj_remove_flag(ic, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
            lv_obj_set_style_bg_opa(ic, LV_OPA_70, LV_STATE_PRESSED);
            lv_obj_t* il = label(ic, a.icon, &font_inter_32);
            lv_obj_center(il);
            lv_obj_t* n = label(cell, a.name, &font_inter_16, color::text());
            lv_obj_align(n, LV_ALIGN_BOTTOM_MID, 0, 0);
            auto make = a.make;
            on_click(cell, [make] { push(make()); });
        }
    }
};

}  // namespace

Screen* make_launcher() { return new Launcher(); }

}  // namespace ui
