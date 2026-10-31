// Alarms (create/edit/enable) and notification history.
#include "app/services.hpp"
#include "app/state.hpp"
#include "core/alarm_schedule.hpp"
#include "ui/ui.hpp"

namespace ui {
namespace {

std::string hhmm(int h, int m) {
    char b[16];
    snprintf(b, sizeof b, "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
    return b;
}

class AlarmEdit : public Screen {
public:
    explicit AlarmEdit(core::Alarm a) : a_(std::move(a)) {}
    const char* title() const override { return a_.id ? "Edit Alarm" : "New Alarm"; }
    void build(lv_obj_t* c) override {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(c, 10, 0);
        lv_obj_set_style_pad_row(c, 10, 0);
        lv_obj_t* row = lv_obj_create(c);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, 380, 170);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 16, 0);
        std::string hours, mins;
        for (int i = 0; i < 24; ++i) hours += (i ? "\n" : "") + std::string(i < 10 ? "0" : "") + std::to_string(i);
        for (int i = 0; i < 60; ++i) mins += (i ? "\n" : "") + std::string(i < 10 ? "0" : "") + std::to_string(i);
        h_ = make_roller(row, hours.c_str(), a_.hour);
        label(row, ":", &font_inter_32);
        m_ = make_roller(row, mins.c_str(), a_.minute);

        lv_obj_t* days = lv_obj_create(c);
        lv_obj_remove_style_all(days);
        lv_obj_set_size(days, LV_PCT(100), 60);
        lv_obj_set_flex_flow(days, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(days, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        static const char* kDays[] = {"S", "M", "T", "W", "T", "F", "S"};
        for (int d = 0; d < 7; ++d) {
            lv_obj_t* b = round_button(days, kDays[d], (a_.days >> d) & 1 ? color::orange() : color::card2(), 50, nullptr);
            on_click(b, [this, b, d] {
                a_.days ^= static_cast<uint8_t>(1u << d);
                lv_obj_set_style_bg_color(b, (a_.days >> d) & 1 ? color::orange() : color::card2(), 0);
            });
        }
        label_row_ = list_row(c, "Label", a_.label.empty() ? "Alarm" : a_.label.c_str(), nullptr, [this] {
            KeyboardOpts o;
            o.label = "Alarm label";
            o.initial = a_.label;
            o.max_len = 24;
            open_keyboard(o, [this](const std::string& s) {
                a_.label = s;
                lv_obj_t* col = lv_obj_get_child(label_row_, 0);
                if (lv_obj_get_child_count(col) > 1) set_text(lv_obj_get_child(col, 1), s.empty() ? "Alarm" : s);
            });
        });
        pill_button(c, "Save", color::orange(), [this] {
            a_.hour = static_cast<uint8_t>(lv_roller_get_selected(h_));
            a_.minute = static_cast<uint8_t>(lv_roller_get_selected(m_));
            a_.enabled = true;
            services::alarm_upsert(a_);
            pop();
        }, 360, 60);
        if (a_.id) {
            uint8_t id = a_.id;
            pill_button(c, "Delete", color::card2(), [id] {
                services::alarm_delete(id);
                pop();
            }, 360, 60);
        }
    }

private:
    lv_obj_t* make_roller(lv_obj_t* parent, const char* opts, int sel) {
        lv_obj_t* r = lv_roller_create(parent);
        lv_roller_set_options(r, opts, LV_ROLLER_MODE_INFINITE);
        lv_roller_set_visible_row_count(r, 3);
        lv_roller_set_selected(r, static_cast<uint32_t>(sel), LV_ANIM_OFF);
        lv_obj_set_width(r, 110);
        lv_obj_set_style_text_font(r, &font_inter_32, 0);
        lv_obj_set_style_bg_color(r, color::bg(), 0);
        lv_obj_set_style_border_width(r, 0, 0);
        lv_obj_set_style_text_color(r, color::muted(), 0);
        lv_obj_set_style_bg_color(r, color::card2(), LV_PART_SELECTED);
        lv_obj_set_style_text_color(r, color::text(), LV_PART_SELECTED);
        return r;
    }
    core::Alarm a_;
    lv_obj_t *h_ = nullptr, *m_ = nullptr, *label_row_ = nullptr;
};

class AlarmsScreen : public Screen {
public:
    const char* title() const override { return "Alarms"; }
    void build(lv_obj_t* c) override {
        c_ = c;
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_row(c, 8, 0);
        refresh(0xFFFFFFFFu);
    }
    void refresh(uint32_t d) override {
        if (!(d & app::kDirtyAlarms)) return;
        clear(c_);
        list_row(c_, "Add alarm", nullptr, nullptr, [] { push(new AlarmEdit(core::Alarm{})); }, color::orange(), LV_SYMBOL_PLUS);
        if (!services::time_valid()) section_header(c_, "Clock not synced: alarms fire once time arrives");
        for (auto& a : services::alarms()) {
            lv_obj_t* r = list_row(c_, hhmm(a.hour, a.minute).c_str(), (a.label.empty() ? core::days_summary(a.days) : a.label + ", " + core::days_summary(a.days)).c_str(),
                                   nullptr, [a] { push(new AlarmEdit(a)); });
            lv_obj_t* sw = lv_switch_create(r);
            lv_obj_set_size(sw, 64, 36);
            lv_obj_set_style_bg_color(sw, color::orange(), sel(LV_PART_INDICATOR, LV_STATE_CHECKED));
            if (a.enabled) lv_obj_add_state(sw, LV_STATE_CHECKED);
            core::Alarm copy = a;
            on(sw, LV_EVENT_VALUE_CHANGED, [copy, sw]() mutable {
                copy.enabled = lv_obj_has_state(sw, LV_STATE_CHECKED);
                services::alarm_upsert(copy);
            });
        }
    }

private:
    lv_obj_t* c_ = nullptr;
};

std::string ago(uint32_t ts) {
    if (!services::time_valid() || ts == 0) return "";
    time_t now = time(nullptr);
    long d = static_cast<long>(now) - static_cast<long>(ts);
    if (d < 60) return "now";
    if (d < 3600) return std::to_string(d / 60) + "m ago";
    if (d < 86400) return std::to_string(d / 3600) + "h ago";
    time_t t = ts;
    struct tm lt;
    localtime_r(&t, &lt);
    char b[16];
    strftime(b, sizeof b, "%b %d", &lt);
    return b;
}

class NotificationDetail : public Screen {
public:
    explicit NotificationDetail(core::StoredNotification n) : n_(std::move(n)) {}
    const char* title() const override { return "Notification"; }
    void build(lv_obj_t* c) override {
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 16, 0);
        lv_obj_set_style_pad_row(c, 10, 0);
        lv_obj_t* t = label(c, n_.title.c_str(), &font_inter_24);
        lv_obj_set_width(t, LV_PCT(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        time_t ts = n_.timestamp;
        struct tm lt;
        localtime_r(&ts, &lt);
        char b[40];
        strftime(b, sizeof b, "%a %b %d, %H:%M", &lt);
        label(c, services::time_valid() ? b : "", &font_inter_16, color::muted());
        lv_obj_t* body = label(c, n_.body.c_str(), &font_inter_20);
        lv_obj_set_width(body, LV_PCT(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
        uint64_t id = n_.id;
        pill_button(c, "Dismiss", color::card2(), [id] {
            {
                app::Locked s;
                s->notifications.dismiss(id);
            }
            services::notifications_changed();
            app::mark(app::kDirtyNotifications);
            pop();
        }, 360, 60);
    }

private:
    core::StoredNotification n_;
};

class NotificationsScreen : public Screen {
public:
    const char* title() const override { return "Notifications"; }
    void build(lv_obj_t* c) override {
        c_ = c;
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(c, 12, 0);
        lv_obj_set_style_pad_row(c, 8, 0);
        {
            app::Locked s;
            s->notifications.mark_all_read();
        }
        refresh(0xFFFFFFFFu);
    }
    void refresh(uint32_t d) override {
        if (!(d & app::kDirtyNotifications)) return;
        clear(c_);
        std::deque<core::StoredNotification> items;
        {
            app::Locked s;
            items = s->notifications.items();
        }
        if (items.empty()) {
            section_header(c_, "No notifications");
            return;
        }
        pill_button(c_, "Clear all", color::card2(), [] {
            {
                app::Locked s;
                s->notifications.clear();
            }
            services::notifications_changed();
            app::mark(app::kDirtyNotifications);
        }, 360, 52);
        for (auto& n : items) {
            core::StoredNotification copy = n;
            lv_obj_t* r = list_row(c_, n.title.c_str(), n.body.c_str(), ago(n.timestamp).c_str(), [copy] { push(new NotificationDetail(copy)); },
                                   n.priority >= 4 ? color::red() : color::purple(), LV_SYMBOL_BELL);
            (void)r;
        }
    }

private:
    lv_obj_t* c_ = nullptr;
};

}  // namespace

Screen* make_alarms() { return new AlarmsScreen(); }
Screen* make_notifications() { return new NotificationsScreen(); }

}  // namespace ui
