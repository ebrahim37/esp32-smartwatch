// Watch UI framework (LVGL 9): theme, navigation stack, reusable widgets and overlays.
//
// Threading: every function here must run in the LVGL task (event callbacks, LVGL timers)
// or with board::lvgl_lock() held.
#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>

#include "lvgl.h"
#include "swp/messages.hpp"

extern "C" {
LV_FONT_DECLARE(font_inter_16)
LV_FONT_DECLARE(font_inter_20)
LV_FONT_DECLARE(font_inter_24)
LV_FONT_DECLARE(font_inter_32)
LV_FONT_DECLARE(font_digits_64)
LV_FONT_DECLARE(font_digits_132)
LV_FONT_DECLARE(font_digits_84)
LV_FONT_DECLARE(font_wx_44)
}

namespace ui {

namespace color {
inline lv_color_t bg() { return lv_color_hex(0x000000); }
inline lv_color_t card() { return lv_color_hex(0x1C1C1E); }
inline lv_color_t card2() { return lv_color_hex(0x2C2C2E); }
inline lv_color_t text() { return lv_color_hex(0xFFFFFF); }
inline lv_color_t muted() { return lv_color_hex(0x8E8E93); }
inline lv_color_t blue() { return lv_color_hex(0x0A84FF); }
inline lv_color_t green() { return lv_color_hex(0x30D158); }
inline lv_color_t red() { return lv_color_hex(0xFF453A); }
inline lv_color_t orange() { return lv_color_hex(0xFF9F0A); }
inline lv_color_t yellow() { return lv_color_hex(0xFFD60A); }
inline lv_color_t purple() { return lv_color_hex(0xBF5AF2); }
inline lv_color_t teal() { return lv_color_hex(0x64D2FF); }
inline lv_color_t pink() { return lv_color_hex(0xFF375F); }
}  // namespace color

#define UI_SYMBOL_SEARCH "\xEF\x80\x82"  // FontAwesome magnifying glass (U+F002)
#define UI_SYMBOL_STAR "\xEF\x80\x85"    // FontAwesome star (U+F005)
#define UI_SYMBOL_LOCK "\xEF\x80\xA3"    // FontAwesome lock (U+F023)

// Safe area for the panel's rounded corners. kCornerR is the visible corner radius of the
// 410x502 AMOLED, measured on the prototype with the Display geometry test patterns: all four
// corners are circular arcs of ~112 px touching the edges, and no edge pixels are hidden.
// Corner-sensitive layout derives from it.
constexpr int kCornerR = 112;
constexpr int kNavBtn = 60;   // Back / Home / Close / Done buttons
constexpr int kNavY = 46;     // nav button centre, from the top edge
constexpr int kCornerMargin = 4;

constexpr int isqrt(int v) {
    int r = 0;
    while ((r + 1) * (r + 1) <= v) ++r;
    return r;
}
// Nav button centre from the side edge: the circle (radius kNavBtn/2 + margin) must lie inside
// the corner arc centred at (kCornerR, kCornerR).
constexpr int kNavX = kCornerR - isqrt((kCornerR - kNavBtn / 2 - kCornerMargin) * (kCornerR - kNavBtn / 2 - kCornerMargin) -
                                       (kCornerR - kNavY) * (kCornerR - kNavY));
constexpr int kTopBarH = kNavY + kNavBtn / 2 + 8;
constexpr int kTitleW = 410 - 2 * (kNavX + kNavBtn / 2) - 12;  // title space between the nav buttons
constexpr int kSafeBottom = 64;  // extra bottom padding for scrollable content

// Places a kNavBtn-sized button in a top corner of `parent` (screen coordinates).
void place_nav(lv_obj_t* btn, bool right);

// Part + state style selector without C++20's deprecated enum-enum arithmetic.
inline lv_style_selector_t sel(lv_part_t p, lv_state_t s) { return static_cast<lv_style_selector_t>(p) | static_cast<lv_style_selector_t>(s); }

class Screen {
public:
    virtual ~Screen() = default;
    virtual const char* title() const { return ""; }
    virtual bool has_top_bar() const { return true; }
    virtual bool scrollable() const { return true; }
    // Builds widgets into `content` (the area below the top bar, or the whole screen).
    virtual void build(lv_obj_t* content) = 0;
    // Called on the UI task with app::Dirty bits that changed since the last call.
    virtual void refresh(uint32_t dirty) { (void)dirty; }
    // Periodic hook while visible; period from tick_ms().
    virtual void tick() {}
    virtual void on_dim(bool dimmed) { (void)dimmed; }  // display dimmed after inactivity / woken
    virtual uint32_t tick_ms() const { return 1000; }
    virtual void on_shown() {}

    lv_obj_t* scr = nullptr;
    lv_obj_t* content = nullptr;
    uint32_t tick_accum = 0;
};

// ---- Navigation ----------------------------------------------------------------------------
void init();  // builds the home screen and starts the UI timer
void push(Screen* s);  // takes ownership
void pop();
void pop_n(int n);  // pops n screens with a single transition
void go_home();
Screen* current();

// ---- Event helpers ---------------------------------------------------------------------------
using Fn = std::function<void()>;
void on(lv_obj_t* obj, lv_event_code_t code, Fn fn);
inline void on_click(lv_obj_t* obj, Fn fn) { on(obj, LV_EVENT_CLICKED, std::move(fn)); }

// ---- Widgets -------------------------------------------------------------------------------
lv_obj_t* label(lv_obj_t* parent, const char* text, const lv_font_t* font = &font_inter_20, lv_color_t c = color::text());
lv_obj_t* pill_button(lv_obj_t* parent, const char* text, lv_color_t bg, Fn fn, int w = LV_SIZE_CONTENT, int h = 64);
lv_obj_t* round_button(lv_obj_t* parent, const char* symbol, lv_color_t bg, int size, Fn fn);
// Full-width list row: title, optional subtitle, optional right-hand value text.
lv_obj_t* list_row(lv_obj_t* parent, const char* title, const char* subtitle = nullptr, const char* value = nullptr, Fn fn = nullptr,
                   lv_color_t icon_bg = color::card2(), const char* icon = nullptr);
lv_obj_t* switch_row(lv_obj_t* parent, const char* title, bool on, std::function<void(bool)> fn);
lv_obj_t* section_header(lv_obj_t* parent, const char* text);
lv_obj_t* card(lv_obj_t* parent);
void clear(lv_obj_t* obj);  // deletes children
void set_text(lv_obj_t* lbl, const std::string& s);  // no-op if unchanged (avoids redraws)

// ---- Overlays ------------------------------------------------------------------------------
void toast(const std::string& text);

// ---- Text entry ----------------------------------------------------------------------------
struct KeyboardOpts {
    std::string label = "Text";
    std::string initial;
    bool password = false;
    bool numeric = false;
    size_t max_len = 64;
};
// Pushes the reusable full-screen keyboard. `on_submit` runs after the keyboard screen has
// been popped; cancel just pops.
void open_keyboard(const KeyboardOpts& opts, std::function<void(const std::string&)> on_submit, Fn on_cancel = nullptr);

// ---- Screen factories ------------------------------------------------------------------
Screen* make_home();
Screen* make_launcher();
Screen* make_music();
Screen* make_now_playing();
Screen* make_maps();
Screen* make_weather();
Screen* make_timer();
Screen* make_stopwatch();
Screen* make_calculator();
Screen* make_alarms();
Screen* make_calendar();
Screen* make_notifications();
Screen* make_settings();
Screen* make_voice_memos();
Screen* make_activity();
Screen* make_geometry_test();
Screen* make_lock();  // Lock app: locks now (or explains how to set a passcode); returns nullptr
Screen* make_passcode_settings();
// Full-screen passcode overlay above everything; stays until the right passcode is entered.
void show_lock_overlay();

// Mini player bar shown at the bottom of music screens while something is playing.
// ---- Weather (ui/weather.cpp) ----------------------------------------------------------------
const char* wx_icon(uint8_t wmo, bool night);  // FontAwesome glyph for a WMO weather code
lv_color_t wx_color(uint8_t wmo, bool night);
bool wx_night(time_t t);
// Summary row: icon, temperature and high/low left of x = `split`, then right-justified the
// outlook (precipitation when coming, else "N UV", red when high) over wind. Compact (dimmed
// watch face): icon and temperature, plus the outlook only when rain is coming or UV is high.
lv_obj_t* weather_row(lv_obj_t* parent, int width, int split);
void weather_row_set(lv_obj_t* row, const swp::Weather& w, bool valid);
void weather_row_compact(lv_obj_t* row, bool compact);
// Thin divider lines (watch face rows, weather row).
lv_obj_t* vdivider(lv_obj_t* parent, int height);
lv_obj_t* hdivider(lv_obj_t* parent, int width);

lv_obj_t* mini_player(lv_obj_t* parent);
void mini_player_refresh(lv_obj_t* bar);

}  // namespace ui
