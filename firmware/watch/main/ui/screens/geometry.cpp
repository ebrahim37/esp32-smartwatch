// Display geometry test patterns (Settings -> Display geometry): full-screen drawings for
// measuring how the bezel and rounded corners crop the 410x502 panel. Tap: next pattern; long
// press: exit.
// Measured on the prototype (2026-11-03): corner radius ~112 px on all four corners, edges fully
// visible (kCornerR in ui.hpp).
#include <cmath>

#include "esp_heap_caps.h"
#include "ui/ui.hpp"

namespace ui {
namespace {

constexpr int kW = 410, kH = 502;

// Colour sequence used for depth bands, arc radii and diagonal decades.
constexpr uint32_t kSeq[] = {0xFF3B30, 0xFF9500, 0xFFD60A, 0x34C759, 0x5AC8FA, 0x0A84FF, 0xBF5AF2, 0xFFFFFF, 0x8E8E93};
constexpr const char* kSeqName[] = {"red", "orange", "yellow", "green", "cyan", "blue", "purple", "white", "grey"};
constexpr int kSeqN = sizeof kSeq / sizeof kSeq[0];

class GeometryTest : public Screen {
public:
    bool has_top_bar() const override { return false; }
    bool scrollable() const override { return false; }
    ~GeometryTest() override {
        if (buf_) heap_caps_free(buf_);
    }
    void build(lv_obj_t* c) override {
        lv_obj_set_style_pad_all(c, 0, 0);
        buf_ = static_cast<uint16_t*>(heap_caps_malloc(kW * kH * 2, MALLOC_CAP_SPIRAM));
        if (!buf_) {
            label(c, "Out of memory", &font_inter_24, color::red());
            return;
        }
        canvas_ = lv_canvas_create(c);
        lv_canvas_set_buffer(canvas_, buf_, kW, kH, LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(canvas_, 0, 0);
        lv_obj_add_flag(canvas_, LV_OBJ_FLAG_CLICKABLE);
        on(canvas_, LV_EVENT_SHORT_CLICKED, [this] {
            page_ = (page_ + 1) % 3;
            draw();
        });
        on(canvas_, LV_EVENT_LONG_PRESSED, [] { pop(); });
        overlay_ = lv_obj_create(c);
        lv_obj_remove_style_all(overlay_);
        lv_obj_set_size(overlay_, kW, kH);
        lv_obj_remove_flag(overlay_, (lv_obj_flag_t)(LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE));
        draw();
    }

private:
    void px(int x, int y, uint32_t rgb) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        buf_[y * kW + x] = static_cast<uint16_t>(((rgb >> 8) & 0xF800) | ((rgb >> 5) & 0x07E0) | ((rgb >> 3) & 0x001F));
    }
    void rect(int x, int y, int w, int h, uint32_t rgb) {
        for (int j = y; j < y + h; ++j)
            for (int i = x; i < x + w; ++i) px(i, j, rgb);
    }
    void text(const std::string& s, int x, int y, const lv_font_t* f = &font_inter_16, lv_color_t col = color::text()) {
        lv_obj_set_pos(label(overlay_, s.c_str(), f, col), x, y);
    }
    void center_text(const std::string& s, int y, const lv_font_t* f = &font_inter_16, lv_color_t col = color::text()) {
        lv_obj_t* l = label(overlay_, s.c_str(), f, col);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, y);
    }

    void draw() {
        rect(0, 0, kW, kH, 0x000000);
        lv_obj_clean(overlay_);
        if (page_ == 0) draw_arcs();
        else if (page_ == 1) draw_edges();
        else draw_grid();
        lv_obj_invalidate(canvas_);
    }

    // 1: corner arcs of radius 80..160 (each corner, centred so the arc touches both edges) and
    // 45-degree diagonal dots coloured per 10 px from the corner.
    void draw_arcs() {
        for (int k = 0; k < kSeqN; ++k) {
            int r = 80 + 10 * k;
            for (int q = 0; q < 4; ++q) {
                int cx = q & 1 ? kW - 1 - r : r, cy = q & 2 ? kH - 1 - r : r;
                int sx = q & 1 ? 1 : -1, sy = q & 2 ? 1 : -1;
                for (int a = 0; a <= 900; ++a) {
                    double t = a * M_PI / 1800.0;
                    px(cx + sx * static_cast<int>(std::lround(r * std::cos(t))), cy + sy * static_cast<int>(std::lround(r * std::sin(t))), kSeq[k]);
                }
            }
        }
        for (int t = 0; t < 90; t += 2)
            for (int q = 0; q < 4; ++q) {
                int x = q & 1 ? kW - 2 - t : t, y = q & 2 ? kH - 2 - t : t;
                rect(x, y, 2, 2, kSeq[t / 10]);
            }
        center_text("1/3  Corner arcs", 150, &font_inter_20);
        center_text("Arc radius 80 red, 90 orange, 100 yellow,", 182);
        center_text("110 green, 120 cyan, 130 blue,", 202);
        center_text("140 purple, 150 white, 160 grey.", 222);
        center_text("Which arcs are complete at each corner?", 250);
        center_text("Diagonal dots from the corner pixel:", 280);
        center_text("0-9 px red, 10-19 orange, 20-29 yellow,", 300);
        center_text("30-39 green, 40-49 cyan ... (2 px steps)", 320);
        center_text("First visible dot colour per corner?", 340);
        center_text("Tap: next   Long press: exit", 380, &font_inter_16, color::muted());
    }

    // 2: edge bars. Bar k is k px long, starting at the edge: the shortest fully visible bar
    // tells how many edge pixels the bezel hides.
    void draw_edges() {
        for (int k = 1; k <= 12; ++k) {
            uint32_t col = k % 2 ? 0xFFFFFF : 0xFFD60A;
            int y = 120 + k * 20;
            rect(0, y, k, 8, col);
            text(std::to_string(k), 22, y - 6);
            rect(kW - k, y, k, 8, col);
            text(std::to_string(k), kW - 40, y - 6);
            int x = 90 + k * 18;
            rect(x, 0, 8, k, col);
            text(std::to_string(k), x - 2, 18);
            rect(x, kH - k, 8, k, col);
            text(std::to_string(k), x - 2, kH - 38);
        }
        center_text("2/3  Edge bars", 210, &font_inter_20);
        center_text("Bar N is N pixels long from the edge.", 242);
        center_text("Smallest bar you can see", 262);
        center_text("on each of the 4 edges?", 282);
        center_text("Tap: next   Long press: exit", 320, &font_inter_16, color::muted());
    }

    // 3: 10 px grid (50 px lines brighter, labelled) and 1 px colour bands at depth 0..8 from
    // every edge, for a photo.
    void draw_grid() {
        for (int x = 0; x < kW; x += 10) rect(x, 0, 1, kH, x % 50 ? 0x2C2C2E : 0x8E8E93);
        for (int y = 0; y < kH; y += 10) rect(0, y, kW, 1, y % 50 ? 0x2C2C2E : 0x8E8E93);
        for (int d = 0; d < kSeqN; ++d) {
            rect(d, 0, 1, kH, kSeq[d]);
            rect(kW - 1 - d, 0, 1, kH, kSeq[d]);
            rect(0, d, kW, 1, kSeq[d]);
            rect(0, kH - 1 - d, kW, 1, kSeq[d]);
        }
        for (int x = 50; x < kW; x += 50) text(std::to_string(x), x + 2, 252, &font_inter_16, color::muted());
        for (int y = 50; y < kH; y += 50) text(std::to_string(y), 207, y + 2, &font_inter_16, color::muted());
        std::string bands;
        for (int d = 0; d < kSeqN; ++d) bands += std::to_string(d) + " " + kSeqName[d] + (d + 1 < kSeqN ? ", " : "");
        center_text("3/3  Grid + edge bands (take a photo)", 120, &font_inter_20);
        lv_obj_t* l = label(overlay_, ("Band depth: " + bands).c_str(), &font_inter_16);
        lv_obj_set_width(l, 300);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, 300);
    }

    uint16_t* buf_ = nullptr;
    lv_obj_t *canvas_ = nullptr, *overlay_ = nullptr;
    int page_ = 0;
};

}  // namespace

Screen* make_geometry_test() { return new GeometryTest(); }

}  // namespace ui
