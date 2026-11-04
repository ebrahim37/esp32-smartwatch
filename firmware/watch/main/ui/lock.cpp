// Passcode lock: the Lock app (locks immediately), Settings -> Passcode (set / change / remove
// the 4-digit passcode) and the full-screen keypad overlay shown while locked. Basic deterrence only: the passcode is kept in
// plain NVS and the lock survives power cycles, not flash access.
#include <string>

#include "app/settings.hpp"
#include "board.hpp"
#include "esp_timer.h"
#include "ui/ui.hpp"

namespace ui {
namespace {

constexpr size_t kPinLen = 4;
constexpr int kMaxTries = 5;               // then a lockout
constexpr int64_t kLockoutUs = 30000000;   // 30 s

lv_obj_t* overlay = nullptr;

struct Keypad {
    std::string entry;
    lv_obj_t* dots[kPinLen] = {};
    lv_obj_t* msg = nullptr;
    int fails = 0;
    int64_t lockout_until = 0;
};
Keypad pad;

void show_dots() {
    for (size_t i = 0; i < kPinLen; ++i)
        lv_obj_set_style_bg_opa(pad.dots[i], i < pad.entry.size() ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void key(char k) {
    int64_t now = esp_timer_get_time();
    if (now < pad.lockout_until) {
        set_text(pad.msg, "Try again in " + std::to_string((pad.lockout_until - now) / 1000000 + 1) + " s");
        return;
    }
    if (k == '<') {
        if (!pad.entry.empty()) pad.entry.pop_back();
        show_dots();
        return;
    }
    if (pad.entry.size() >= kPinLen) return;
    pad.entry += k;
    show_dots();
    if (pad.entry.size() < kPinLen) return;
    if (pad.entry == settings::lock_pin()) {
        settings::set_locked(false);
        board::vibrate(40);
        lv_obj_delete(overlay);
        overlay = nullptr;
        return;
    }
    board::vibrate(250);
    pad.entry.clear();
    show_dots();
    if (++pad.fails >= kMaxTries) {
        pad.fails = 0;
        pad.lockout_until = now + kLockoutUs;
        set_text(pad.msg, "Too many attempts: wait 30 s");
    } else {
        set_text(pad.msg, "Wrong passcode");
    }
}

// Asks for a new 4-digit passcode twice; `done` runs after it is saved.
void choose_passcode(Fn done) {
    KeyboardOpts o;
    o.label = "New 4-digit passcode";
    o.numeric = true;
    o.password = true;
    o.max_len = kPinLen;
    open_keyboard(o, [done](const std::string& first) {
        if (first.size() != kPinLen) {
            toast("Passcode must be 4 digits");
            return;
        }
        KeyboardOpts c;
        c.label = "Repeat passcode";
        c.numeric = true;
        c.password = true;
        c.max_len = kPinLen;
        open_keyboard(c, [first, done](const std::string& second) {
            if (second != first) {
                toast("Passcodes didn't match");
                return;
            }
            settings::set_lock_pin(first);
            toast("Passcode set");
            if (done) done();
        });
    });
}

// Asks for the current passcode, then runs `ok`.
void verify_passcode(const char* label, Fn ok) {
    KeyboardOpts o;
    o.label = label;
    o.numeric = true;
    o.password = true;
    o.max_len = kPinLen;
    open_keyboard(o, [ok](const std::string& pin) {
        if (pin != settings::lock_pin()) {
            board::vibrate(250);
            toast("Wrong passcode");
            return;
        }
        ok();
    });
}

void lock_now() {
    settings::set_locked(true);
    show_lock_overlay();
}

class PasscodeScreen : public Screen {
public:
    const char* title() const override { return "Passcode"; }
    void build(lv_obj_t* c) override {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_row(c, 10, 0);
        rebuild();
    }
    void refresh(uint32_t d) override {
        if (d == 0xFFFFFFFFu) rebuild();  // back from the passcode keyboard
    }

private:
    void rebuild() {
        clear(content);
        bool has = !settings::lock_pin().empty();
        if (!has) {
            section_header(content, "Set a 4-digit passcode, then lock the watch from the Lock app. The lock stays on after a restart.");
            list_row(content, "Set passcode", nullptr, nullptr, [] { choose_passcode(nullptr); }, color::blue(), UI_SYMBOL_LOCK);
            return;
        }
        section_header(content, "Lock the watch from the Lock app.");
        list_row(content, "Change passcode", nullptr, nullptr, [] { verify_passcode("Current passcode", [] { choose_passcode(nullptr); }); },
                 color::card2(), LV_SYMBOL_EDIT);
        list_row(content, "Turn passcode off", nullptr, nullptr, [this] {
            verify_passcode("Current passcode", [this] {
                settings::set_lock_pin("");
                settings::set_locked(false);
                toast("Passcode off");
                rebuild();
            });
        }, color::red(), LV_SYMBOL_CLOSE);
    }
};

}  // namespace

void show_lock_overlay() {
    if (overlay) return;
    pad.entry.clear();
    overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(overlay, color::bg(), 0);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);  // swallow touches meant for the screen below
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* icon = label(overlay, UI_SYMBOL_LOCK, &font_inter_32);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 30);
    lv_obj_t* t = label(overlay, "Enter passcode", &font_inter_20);
    lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 72);
    for (size_t i = 0; i < kPinLen; ++i) {
        lv_obj_t* d = lv_obj_create(overlay);
        lv_obj_remove_style_all(d);
        lv_obj_set_size(d, 16, 16);
        lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(d, 2, 0);
        lv_obj_set_style_border_color(d, color::text(), 0);
        lv_obj_set_style_bg_color(d, color::text(), 0);
        lv_obj_align(d, LV_ALIGN_TOP_MID, (static_cast<int>(i) - 1) * 32 - 16, 106);
        pad.dots[i] = d;
    }
    pad.msg = label(overlay, "", &font_inter_16, color::orange());
    lv_obj_align(pad.msg, LV_ALIGN_TOP_MID, 0, 128);
    show_dots();

    // 3x4 keypad: 1-9, (blank), 0, delete.
    static const char kKeys[] = "123456789 0<";
    for (int i = 0; i < 12; ++i) {
        char k = kKeys[i];
        if (k == ' ') continue;
        char txt[2] = {k, 0};
        lv_obj_t* b = round_button(overlay, k == '<' ? LV_SYMBOL_BACKSPACE : txt, k == '<' ? color::bg() : color::card2(), 72,
                                   [k] { key(k); });
        lv_obj_align(b, LV_ALIGN_TOP_MID, (i % 3 - 1) * 90, 154 + (i / 3) * 80);
    }
}

Screen* make_lock() {
    if (settings::lock_pin().empty()) toast("Set a passcode in Settings \xE2\x86\x92 Passcode");  // U+2192 arrow
    else lock_now();
    return nullptr;  // no screen: the Lock app acts immediately
}

Screen* make_passcode_settings() { return new PasscodeScreen(); }

}  // namespace ui
