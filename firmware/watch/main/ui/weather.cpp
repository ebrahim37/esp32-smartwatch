// Weather presentation shared by the Weather app and the watch face: condition icons and the
// summary row (icon, temperature, high/low, precipitation or UV, wind).
#include <cmath>
#include <ctime>

#include "core/format.hpp"
#include "ui/ui.hpp"

namespace ui {
namespace {

// FontAwesome 5 Solid glyphs (in the text fonts and font_wx_52).
constexpr const char* kSun = "\xEF\x86\x85";        // U+F185
constexpr const char* kMoon = "\xEF\x86\x86";       // U+F186
constexpr const char* kCloud = "\xEF\x83\x82";      // U+F0C2
constexpr const char* kCloudSun = "\xEF\x9B\x84";   // U+F6C4
constexpr const char* kCloudMoon = "\xEF\x9B\x83";  // U+F6C3
constexpr const char* kRain = "\xEF\x9C\xBD";       // U+F73D
constexpr const char* kShowers = "\xEF\x9D\x80";    // U+F740
constexpr const char* kSnow = "\xEF\x8B\x9C";       // U+F2DC
constexpr const char* kFog = "\xEF\x9D\x9F";        // U+F75F
constexpr const char* kBolt = "\xEF\x83\xA7";       // U+F0E7

struct Row {
    lv_obj_t *icon, *temp, *hl, *high, *low, *wind, *precip;
    bool compact = false;
    bool notable = false;  // precipitation coming, or high UV: still shown when compact
};

void apply_compact(Row* r) {
    for (lv_obj_t* o : {r->hl, r->wind}) {
        if (r->compact) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
    if (r->compact && !r->notable) lv_obj_add_flag(r->precip, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(r->precip, LV_OBJ_FLAG_HIDDEN);
}

}  // namespace

const char* wx_icon(uint8_t wmo, bool night) {
    if (wmo == 0) return night ? kMoon : kSun;
    if (wmo <= 2) return night ? kCloudMoon : kCloudSun;
    if (wmo == 3) return kCloud;
    if (wmo == 45 || wmo == 48) return kFog;
    if (wmo >= 51 && wmo <= 57) return kRain;
    if (wmo == 65 || wmo == 67 || wmo == 81 || wmo == 82) return kShowers;
    if ((wmo >= 61 && wmo <= 67) || wmo == 80) return kRain;
    if ((wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86) return kSnow;
    if (wmo >= 95) return kBolt;
    return kCloud;
}

lv_color_t wx_color(uint8_t wmo, bool night) {
    if (wmo <= 2) return night ? lv_color_hex(0xE5E5EA) : color::yellow();
    if (wmo >= 95) return color::yellow();
    if ((wmo >= 51 && wmo <= 67) || (wmo >= 80 && wmo <= 82)) return color::teal();
    if ((wmo >= 71 && wmo <= 77) || wmo == 85 || wmo == 86) return color::text();
    return lv_color_hex(0xC7C7CC);  // cloud, fog
}

bool wx_night(time_t t) {
    struct tm lt;
    localtime_r(&t, &lt);
    return lt.tm_hour < 6 || lt.tm_hour >= 20;  // TODO(PROTOTYPE): sunrise/sunset from the forecast
}

lv_obj_t* weather_row(lv_obj_t* parent, int width, int split) {
    constexpr int kH = 56;
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, width, kH);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);
    auto* r = new Row{};

    // Left: icon (inset to line up with the watch face's date and time), temperature, H/L.
    lv_obj_t* left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_size(left, split, kH);
    lv_obj_set_style_pad_left(left, 18, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(left, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(left, LV_OBJ_FLAG_EVENT_BUBBLE);
    r->icon = label(left, "", &font_wx_44);
    lv_obj_set_style_margin_right(r->icon, 8, 0);
    r->temp = label(left, "--", &font_digits_64);
    lv_obj_set_style_margin_right(r->temp, 10, 0);
    r->hl = lv_obj_create(left);
    lv_obj_remove_style_all(r->hl);
    lv_obj_set_size(r->hl, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r->hl, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(r->hl, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    r->high = label(r->hl, "", &font_inter_20, color::red());
    r->low = label(r->hl, "", &font_inter_20, color::blue());

    // Right: wind and precipitation outlook, right-justified.
    lv_obj_t* right = lv_obj_create(row);
    lv_obj_remove_style_all(right);
    lv_obj_set_size(right, width - split, kH);
    lv_obj_set_pos(right, split, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
    lv_obj_remove_flag(right, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
    lv_obj_add_flag(right, LV_OBJ_FLAG_EVENT_BUBBLE);
    for (lv_obj_t** l : {&r->precip, &r->wind}) {  // outlook (rain or UV) above wind
        *l = label(right, "", &font_inter_16);
        lv_obj_set_width(*l, LV_PCT(100));
        lv_obj_set_style_text_align(*l, LV_TEXT_ALIGN_RIGHT, 0);
        lv_label_set_long_mode(*l, LV_LABEL_LONG_DOT);
    }

    lv_obj_set_user_data(row, r);
    lv_obj_add_event_cb(
        row, [](lv_event_t* e) { delete static_cast<Row*>(lv_event_get_user_data(e)); }, LV_EVENT_DELETE, r);
    return row;
}

void weather_row_compact(lv_obj_t* row, bool compact) {
    auto* r = static_cast<Row*>(lv_obj_get_user_data(row));
    if (!r) return;
    r->compact = compact;
    apply_compact(r);
}

void weather_row_set(lv_obj_t* row, const swp::Weather& w, bool valid) {
    auto* r = static_cast<Row*>(lv_obj_get_user_data(row));
    if (!r) return;
    if (!valid) {
        set_text(r->icon, "");
        set_text(r->temp, "--");
        set_text(r->high, "");
        set_text(r->low, "");
        set_text(r->wind, "Weather");
        set_text(r->precip, "unavailable");
        lv_obj_set_style_text_color(r->precip, color::muted(), 0);
        r->notable = false;
        apply_compact(r);
        return;
    }
    bool night = wx_night(time(nullptr));
    set_text(r->icon, wx_icon(w.wmo_code, night));
    lv_obj_set_style_text_color(r->icon, wx_color(w.wmo_code, night), 0);
    set_text(r->temp, core::fmt_temp(w.temp_c10) + "\xC2\xB0");
    set_text(r->high, core::fmt_temp(w.high_c10) + "\xC2\xB0");
    set_text(r->low, core::fmt_temp(w.low_c10) + "\xC2\xB0");
    set_text(r->wind, "Wind " + std::to_string(static_cast<int>(std::lround(w.wind_kmh10 / 10.0))) + " km/h");
    // Outlook: precipitation when it is coming, otherwise the UV index (red when high).
    bool coming = w.precip_kind != 0 && w.precip_kind != 0xFF && w.precip_in_min >= 0;
    float uv = w.uv10 / 10.f;
    bool uv_high = core::uv_level(uv) >= core::Level::High;
    if (coming) set_text(r->precip, core::fmt_precip(w.precip_kind, w.precip_in_min, w.precip_horizon_h));
    else set_text(r->precip, std::to_string(static_cast<int>(std::lround(uv))) + " UV");
    lv_obj_set_style_text_color(r->precip, coming ? color::blue() : uv_high ? color::red() : color::text(), 0);
    r->notable = coming || uv_high;
    apply_compact(r);
}

}  // namespace ui
