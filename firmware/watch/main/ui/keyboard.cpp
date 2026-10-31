// Reusable full-screen text entry: black background, small field label, blinking cursor,
// QWERTY / symbols / numeric layouts, shift, backspace, submit and cancel.
#include "ui/ui.hpp"

namespace ui {
namespace {

// Apple Watch-style layout: Cancel / Done in the top corners, backspace at the end of the
// text field, the layout switch where backspace used to be, and a space bar on its own row.
// The space row is padded with hidden keys so it stays clear of the rounded bottom corners.
#define KB_SPACE_ROW " ", "space", " ", ""
const char* kLower[] = {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
                        "a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
                        LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", "123", "\n",
                        KB_SPACE_ROW};
const char* kUpper[] = {"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
                        "A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
                        LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", "123", "\n",
                        KB_SPACE_ROW};
const char* kSymbols[] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0", "\n",
                          "-", "/", ":", ";", "(", ")", "$", "&", "@", "\"", "\n",
                          "#+=", ".", ",", "?", "!", "'", "_", "ABC", "\n",
                          KB_SPACE_ROW};
const char* kSymbols2[] = {"[", "]", "{", "}", "#", "%", "^", "*", "+", "=", "\n",
                           "_", "\\", "|", "~", "<", ">", "`", "\u00b0", "\u2022", "\n",
                           "123", ".", ",", "?", "!", "'", "\u2026", "-", "ABC", "\n",
                           KB_SPACE_ROW};
#undef KB_SPACE_ROW
const char* kDigits[] = {"1", "2", "3", "\n", "4", "5", "6", "\n", "7", "8", "9", "\n", " ", "0", " ", ""};

constexpr uint32_t kShiftId = 19;  // first key of the third row in the text layouts
constexpr lv_buttonmatrix_ctrl_t H = LV_BUTTONMATRIX_CTRL_HIDDEN;
inline lv_buttonmatrix_ctrl_t W(int w) { return static_cast<lv_buttonmatrix_ctrl_t>(w); }
// Mode keys (shift, layout switches) must not auto-repeat while held.
inline lv_buttonmatrix_ctrl_t M(int w) { return static_cast<lv_buttonmatrix_ctrl_t>(w | LV_BUTTONMATRIX_CTRL_NO_REPEAT); }
constexpr uint32_t kCapsDoubleTapMs = 400;

// Width weights: rows 1-2 one unit per key; row 3 has wide mode keys either side; the space
// bar is centred between hidden spacers. No key is CHECKABLE, so nothing latches blue.
const lv_buttonmatrix_ctrl_t kCtrlText[] = {
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    M(3), W(2), W(2), W(2), W(2), W(2), W(2), W(2), M(3),
    static_cast<lv_buttonmatrix_ctrl_t>(H | 2), W(6), static_cast<lv_buttonmatrix_ctrl_t>(H | 2)};
// Numbers layout: ten keys in row 2 (ends with the double quote), six in row 3.
const lv_buttonmatrix_ctrl_t kCtrlSymbols[] = {
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1),
    M(3), W(2), W(2), W(2), W(2), W(2), W(2), M(3),
    static_cast<lv_buttonmatrix_ctrl_t>(H | 2), W(6), static_cast<lv_buttonmatrix_ctrl_t>(H | 2)};
const lv_buttonmatrix_ctrl_t kCtrlDigits[] = {W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), W(1), H, W(1), H};

class KeyboardScreen : public Screen {
public:
    KeyboardScreen(const KeyboardOpts& o, std::function<void(const std::string&)> submit, Fn cancel)
        : opts_(o), submit_(std::move(submit)), cancel_(std::move(cancel)) {}
    bool has_top_bar() const override { return false; }
    bool scrollable() const override { return false; }

    void build(lv_obj_t* c) override {
        place_nav(round_button(c, LV_SYMBOL_CLOSE, color::card(), kNavBtn, [this] { finish(false); }), false);
        lv_obj_t* ok = round_button(c, LV_SYMBOL_OK, color::blue(), kNavBtn, [this] { finish(true); });
        place_nav(ok, true);
        lv_obj_t* l = label(c, opts_.label.c_str(), &font_inter_20, color::muted());
        lv_obj_set_width(l, kTitleW);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(l, LV_ALIGN_TOP_MID, 0, kNavY - 12);

        constexpr int kFieldY = kTopBarH + 2, kFieldH = 64, kBsp = 60;
        ta_ = lv_textarea_create(c);
        lv_obj_set_size(ta_, 384 - kBsp - 8, kFieldH);
        lv_obj_align(ta_, LV_ALIGN_TOP_LEFT, 13, kFieldY);
        lv_textarea_set_one_line(ta_, true);
        lv_textarea_set_max_length(ta_, static_cast<uint32_t>(opts_.max_len));
        lv_textarea_set_password_mode(ta_, opts_.password);
        lv_textarea_set_password_show_time(ta_, 1200);
        lv_textarea_set_text(ta_, opts_.initial.c_str());
        lv_obj_set_style_bg_color(ta_, color::bg(), 0);
        lv_obj_set_style_border_side(ta_, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(ta_, 2, 0);
        lv_obj_set_style_border_color(ta_, color::card2(), 0);
        lv_obj_set_style_text_font(ta_, opts_.numeric ? &font_inter_32 : &font_inter_24, 0);
        lv_obj_set_style_text_color(ta_, color::text(), 0);
        lv_obj_set_style_bg_color(ta_, color::blue(), sel(LV_PART_CURSOR, LV_STATE_FOCUSED));
        lv_obj_set_style_border_color(ta_, color::blue(), sel(LV_PART_CURSOR, LV_STATE_FOCUSED));
        lv_obj_set_style_anim_duration(ta_, 500, sel(LV_PART_CURSOR, LV_STATE_FOCUSED));  // cursor blink period
        lv_obj_add_state(ta_, LV_STATE_FOCUSED);
        lv_obj_t* bsp = round_button(c, LV_SYMBOL_BACKSPACE, color::card2(), kBsp, [this] { lv_textarea_delete_char(ta_); });
        lv_obj_align_to(bsp, ta_, LV_ALIGN_OUT_RIGHT_MID, 8, 0);

        kb_ = lv_buttonmatrix_create(c);
        int kb_top = kFieldY + kFieldH + 8;
        lv_obj_set_size(kb_, 400, 502 - kb_top - 10);
        lv_obj_align(kb_, LV_ALIGN_TOP_MID, 0, kb_top);
        lv_obj_set_style_bg_opa(kb_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(kb_, 0, 0);
        lv_obj_set_style_pad_all(kb_, 2, 0);
        lv_obj_set_style_pad_gap(kb_, 6, 0);
        lv_obj_set_style_bg_color(kb_, color::card2(), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(kb_, color::muted(), sel(LV_PART_ITEMS, LV_STATE_PRESSED));
        lv_obj_set_style_bg_color(kb_, color::muted(), sel(LV_PART_ITEMS, LV_STATE_CHECKED));
        lv_obj_set_style_text_color(kb_, color::text(), LV_PART_ITEMS);
        lv_obj_set_style_text_font(kb_, opts_.numeric ? &font_inter_32 : &font_inter_24, LV_PART_ITEMS);
        lv_obj_set_style_radius(kb_, 12, LV_PART_ITEMS);
        lv_obj_set_style_shadow_width(kb_, 0, LV_PART_ITEMS);
        lv_obj_set_style_border_width(kb_, 0, LV_PART_ITEMS);
        set_layout(opts_.numeric ? kDigits : kLower);
        lv_obj_add_event_cb(kb_, on_key, LV_EVENT_VALUE_CHANGED, this);
        lv_obj_add_event_cb(kb_, on_draw, LV_EVENT_DRAW_TASK_ADDED, this);
        lv_obj_add_flag(kb_, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    }

private:
    void set_layout(const char** map) {
        map_ = map;
        lv_buttonmatrix_set_map(kb_, map);
        lv_buttonmatrix_set_ctrl_map(kb_, map == kDigits ? kCtrlDigits : map == kSymbols ? kCtrlSymbols : kCtrlText);
        if (map != kUpper) caps_lock_ = false;
        // Shift: grey while upper case is active; caps lock is drawn white by on_draw.
        if (map == kUpper) lv_buttonmatrix_set_button_ctrl(kb_, kShiftId, LV_BUTTONMATRIX_CTRL_CHECKED);
    }

    // Shift: tap = next letter upper case; double tap = caps lock; tap again = lower case.
    void on_shift() {
        uint32_t now = lv_tick_get();
        if (map_ == kUpper && !caps_lock_ && now - last_shift_ms_ < kCapsDoubleTapMs) {
            caps_lock_ = true;
            lv_obj_invalidate(kb_);
        } else {
            set_layout(map_ == kUpper ? kLower : kUpper);
        }
        last_shift_ms_ = now;
    }

    static void on_draw(lv_event_t* e) {
        auto* self = static_cast<KeyboardScreen*>(lv_event_get_user_data(e));
        if (!self->caps_lock_) return;
        lv_draw_task_t* t = lv_event_get_draw_task(e);
        auto* base = static_cast<lv_draw_dsc_base_t*>(lv_draw_task_get_draw_dsc(t));
        if (base->part != LV_PART_ITEMS || base->id1 != kShiftId) return;
        if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_FILL) static_cast<lv_draw_fill_dsc_t*>(lv_draw_task_get_draw_dsc(t))->color = color::text();
        if (lv_draw_task_get_type(t) == LV_DRAW_TASK_TYPE_LABEL) static_cast<lv_draw_label_dsc_t*>(lv_draw_task_get_draw_dsc(t))->color = color::bg();
    }

    static void on_key(lv_event_t* e) {
        auto* self = static_cast<KeyboardScreen*>(lv_event_get_user_data(e));
        uint32_t id = lv_buttonmatrix_get_selected_button(self->kb_);
        if (id == LV_BUTTONMATRIX_BUTTON_NONE) return;
        const char* t = lv_buttonmatrix_get_button_text(self->kb_, id);
        if (!t) return;
        std::string k = t;
        if (k == " ") return;  // hidden spacer
        if (k == LV_SYMBOL_UP) return self->on_shift();
        if (k == "123") return self->set_layout(kSymbols);
        if (k == "#+=") return self->set_layout(kSymbols2);
        if (k == "ABC") return self->set_layout(kLower);
        if (k == "space") k = " ";
        lv_textarea_add_text(self->ta_, k.c_str());
        if (self->map_ == kUpper && !self->caps_lock_) self->set_layout(kLower);  // one-shot shift
    }

    void finish(bool ok) {
        std::string text = lv_textarea_get_text(ta_);
        auto submit = std::move(submit_);
        auto cancel = std::move(cancel_);
        pop();  // `this` stays alive until the slide-out animation deletes the screen
        if (ok && submit) submit(text);
        if (!ok && cancel) cancel();
    }

    KeyboardOpts opts_;
    std::function<void(const std::string&)> submit_;
    Fn cancel_;
    lv_obj_t* ta_ = nullptr;
    lv_obj_t* kb_ = nullptr;
    const char** map_ = nullptr;
    bool caps_lock_ = false;
    uint32_t last_shift_ms_ = 0;
};

}  // namespace

void open_keyboard(const KeyboardOpts& opts, std::function<void(const std::string&)> on_submit, Fn on_cancel) {
    push(new KeyboardScreen(opts, std::move(on_submit), std::move(on_cancel)));
}

}  // namespace ui
