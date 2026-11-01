#include "app/services.hpp"

#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>

#include "app/settings.hpp"
#include "app/state.hpp"
#include "ble/link.hpp"
#include "core/heading.hpp"
#include "core/step_counter.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace services {
namespace {

constexpr const char* TAG = "svc";

std::mutex mu;  // guards everything below that is not atomic
core::StepCounter step_counter;
core::RelativeHeading heading;
std::atomic<bool> heading_reset_req{false};
std::atomic<float> heading_cache{0.f};
std::atomic<uint32_t> steps_cache{0};
int32_t steps_day_key = 0;
uint32_t steps_saved = 0;
board::PowerStatus power_cache;

TimerInfo timer_info;
int64_t timer_end_us = 0;

StopwatchInfo sw;
int64_t sw_started_us = 0;
int64_t sw_accum_us = 0;

std::vector<core::Alarm> alarm_list;
core::AlarmScheduler scheduler;
struct Snooze {
    uint8_t id;
    time_t at;
};
std::vector<Snooze> snoozes;

std::atomic<int64_t> notif_dirty_at{0};

int32_t day_key(const struct tm& lt) { return (lt.tm_year + 1900) * 10000 + (lt.tm_mon + 1) * 100 + lt.tm_mday; }

void sensor_task(void*) {
    // 50 Hz is enough for gait (<3 Hz) and wrist rotation; vTaskDelayUntil keeps it periodic
    // without busy-waiting.
    TickType_t last = xTaskGetTickCount();
    board::ImuSample s;
    int ticks = 0;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(20));
        if (!board::imu_read(s)) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            last = xTaskGetTickCount();
            continue;
        }
        if (heading_reset_req.exchange(false)) heading.reset(0.f);
        heading.update(s.ax, s.ay, s.az, s.gx, s.gy, s.gz, s.t_us);
        heading_cache.store(heading.degrees());
        {
            std::lock_guard<std::mutex> g(mu);
            if (step_counter.add_sample(s.ax, s.ay, s.az, s.t_us)) steps_cache.store(step_counter.total());
        }
        if (++ticks % 50 == 0) app::mark(app::kDirtySensors);
    }
}

void fire_alert(app::Alert::Kind kind, const std::string& title, const std::string& body, uint8_t alarm_id, uint8_t pulses) {
    app::Alert a;
    a.kind = kind;
    a.title = title;
    a.body = body;
    a.alarm_id = alarm_id;
    app::post_alert(a);
    notify_feedback(pulses, kind == app::Alert::Timer || kind == app::Alert::Alarm);
}

void tick_timer() {
    bool done = false;
    {
        std::lock_guard<std::mutex> g(mu);
        if (timer_info.state == TimerState::Running) {
            timer_info.remaining_ms = std::max<int64_t>(0, (timer_end_us - esp_timer_get_time()) / 1000);
            if (timer_info.remaining_ms == 0) {
                timer_info.state = TimerState::Done;
                done = true;
            }
        }
    }
    if (done) {
        fire_alert(app::Alert::Timer, "Timer", "Time's up", 0, 6);
        app::mark(app::kDirtyTimer);
    }
}

void tick_alarms(time_t now) {
    std::vector<std::pair<uint8_t, std::string>> fired;
    bool changed = false;
    {
        std::lock_guard<std::mutex> g(mu);
        auto ids = scheduler.poll(alarm_list, now);
        for (auto id : ids)
            for (auto& a : alarm_list)
                if (a.id == id) {
                    fired.emplace_back(id, a.label);
                    changed |= a.days == 0;
                }
        for (auto it = snoozes.begin(); it != snoozes.end();) {
            if (it->at <= now) {
                fired.emplace_back(it->id, "Snoozed alarm");
                it = snoozes.erase(it);
            } else {
                ++it;
            }
        }
        if (changed) settings::set_alarms(alarm_list);
    }
    for (auto& f : fired) fire_alert(app::Alert::Alarm, "Alarm", f.second.empty() ? "Alarm" : f.second, f.first, 10);
    if (changed) app::mark(app::kDirtyAlarms);
}

void tick_steps(const struct tm& lt, bool valid, int64_t now_us) {
    static int64_t last_save_us = 0;
    std::lock_guard<std::mutex> g(mu);
    if (valid) {
        int32_t dk = day_key(lt);
        if (steps_day_key == 0) {
            steps_day_key = dk;
        } else if (dk != steps_day_key) {
            // Local date rollover: start a new day's count.
            ESP_LOGI(TAG, "day rollover: %lu steps on %ld", (unsigned long)step_counter.total(), (long)steps_day_key);
            step_counter.set_total(0);
            steps_cache.store(0);
            steps_day_key = dk;
            settings::set_steps({0, dk});
            steps_saved = 0;
            last_save_us = now_us;
        }
    }
    // Checkpoint at most every 10 minutes, and only if the count changed.
    if (step_counter.total() != steps_saved && now_us - last_save_us > 600LL * 1000000) {
        settings::set_steps({step_counter.total(), steps_day_key});
        steps_saved = step_counter.total();
        last_save_us = now_us;
    }
}

void persist_notifications_if_due(int64_t now_us) {
    int64_t at = notif_dirty_at.load();
    if (at == 0 || now_us - at < 30LL * 1000000) return;
    notif_dirty_at.store(0);
    std::vector<uint8_t> blob;
    {
        app::Locked s;
        blob = s->notifications.serialize();
    }
    settings::set_blob("notif", blob);
}

void service_task(void*) {
    TickType_t last = xTaskGetTickCount();
    int n = 0;
    int last_min = -1;
    for (;;) {
        vTaskDelayUntil(&last, pdMS_TO_TICKS(250));
        int64_t now_us = esp_timer_get_time();
        tick_timer();
        if (++n % 4 != 0) continue;  // remaining work at 1 Hz

        struct tm lt;
        bool valid = local_now(lt);
        time_t now = time(nullptr);
        tick_alarms(now);
        tick_steps(lt, valid, now_us);
        persist_notifications_if_due(now_us);
        if (valid && lt.tm_min != last_min) {
            last_min = lt.tm_min;
            app::mark(app::kDirtyTime);
        }
        if (n % 120 == 0 || n == 4) {  // power telemetry every 30 s
            auto p = board::power();
            std::lock_guard<std::mutex> g(mu);
            power_cache = p;
        }
        {
            std::lock_guard<std::mutex> g(mu);
            if (sw.running) app::mark(app::kDirtyTimer);
        }
    }
}

}  // namespace

void start() {
    alarm_list = settings::alarms();
    auto cp = settings::steps();
    struct tm lt;
    if (local_now(lt) && cp.day_key == day_key(lt)) {
        step_counter.set_total(cp.steps);
        steps_day_key = cp.day_key;
    } else if (!local_now(lt)) {
        // Wall clock unknown after a cold boot: keep the checkpoint until time arrives.
        step_counter.set_total(cp.steps);
        steps_day_key = cp.day_key;
    }
    steps_saved = step_counter.total();
    steps_cache.store(step_counter.total());
    set_step_threshold_mg(settings::get().step_threshold_mg);

    auto nb = settings::blob("notif");
    if (!nb.empty()) {
        app::Locked s;
        s->notifications.deserialize(nb);
    }
    // Core 1 shares with LVGL; priorities keep the sensor sampling ahead of rendering.
    xTaskCreatePinnedToCore(sensor_task, "imu", 4096, nullptr, 6, nullptr, 1);
    xTaskCreatePinnedToCore(service_task, "svc", 6144, nullptr, 3, nullptr, 1);
}

bool time_valid() { return time(nullptr) >= core::AlarmScheduler::kMinValidUtc; }

void apply_time_sync(uint64_t utc_ms) {
    if (utc_ms / 1000 < static_cast<uint64_t>(core::AlarmScheduler::kMinValidUtc)) return;
    struct timeval tv;
    tv.tv_sec = static_cast<time_t>(utc_ms / 1000);
    tv.tv_usec = static_cast<suseconds_t>((utc_ms % 1000) * 1000);
    struct timeval cur;
    gettimeofday(&cur, nullptr);
    int64_t drift_ms = (static_cast<int64_t>(cur.tv_sec) * 1000 + cur.tv_usec / 1000) - static_cast<int64_t>(utc_ms);
    settimeofday(&tv, nullptr);
    ESP_LOGI(TAG, "time synchronised (correction %lld ms)", (long long)-drift_ms);
    app::mark(app::kDirtyTime);
}

bool local_now(struct tm& out) {
    time_t now = time(nullptr);
    localtime_r(&now, &out);
    return time_valid();
}

uint32_t steps_today() { return steps_cache.load(); }
float heading_deg() { return heading_cache.load(); }
void reset_heading() {
    heading_reset_req.store(true);
    heading_cache.store(0.f);
}

void set_step_threshold_mg(uint16_t mg) {
    std::lock_guard<std::mutex> g(mu);
    auto cfg = step_counter.config();
    cfg.threshold_g = std::clamp(mg, static_cast<uint16_t>(1050), static_cast<uint16_t>(2000)) / 1000.f - 1.0f;
    step_counter.set_config(cfg);
}

board::PowerStatus power() {
    std::lock_guard<std::mutex> g(mu);
    return power_cache;
}

TimerInfo timer() {
    std::lock_guard<std::mutex> g(mu);
    TimerInfo t = timer_info;
    // Live value: the 250 ms service tick would otherwise make the display lag.
    if (t.state == TimerState::Running) t.remaining_ms = std::max<int64_t>(0, (timer_end_us - esp_timer_get_time()) / 1000);
    return t;
}

void timer_set(uint32_t seconds) {
    std::lock_guard<std::mutex> g(mu);
    seconds = std::clamp<uint32_t>(seconds, 1, 24 * 3600 - 1);
    timer_info = {TimerState::Idle, seconds, static_cast<int64_t>(seconds) * 1000};
    app::mark(app::kDirtyTimer);
}

void timer_start() {
    std::lock_guard<std::mutex> g(mu);
    if (timer_info.state == TimerState::Done || timer_info.remaining_ms <= 0) timer_info.remaining_ms = timer_info.duration_s * 1000LL;
    timer_end_us = esp_timer_get_time() + timer_info.remaining_ms * 1000;
    timer_info.state = TimerState::Running;
    app::mark(app::kDirtyTimer);
}

void timer_pause() {
    std::lock_guard<std::mutex> g(mu);
    if (timer_info.state != TimerState::Running) return;
    timer_info.remaining_ms = std::max<int64_t>(0, (timer_end_us - esp_timer_get_time()) / 1000);
    timer_info.state = TimerState::Paused;
    app::mark(app::kDirtyTimer);
}

void timer_reset() {
    std::lock_guard<std::mutex> g(mu);
    timer_info.state = TimerState::Idle;
    timer_info.remaining_ms = timer_info.duration_s * 1000LL;
    app::mark(app::kDirtyTimer);
}

StopwatchInfo stopwatch() {
    std::lock_guard<std::mutex> g(mu);
    StopwatchInfo s = sw;
    s.elapsed_us = sw_accum_us + (sw.running ? esp_timer_get_time() - sw_started_us : 0);
    return s;
}

void stopwatch_toggle() {
    std::lock_guard<std::mutex> g(mu);
    int64_t now = esp_timer_get_time();
    if (sw.running) sw_accum_us += now - sw_started_us;
    else sw_started_us = now;
    sw.running = !sw.running;
    app::mark(app::kDirtyTimer);
}

void stopwatch_lap() {
    std::lock_guard<std::mutex> g(mu);
    if (!sw.running) return;
    int64_t el = sw_accum_us + (esp_timer_get_time() - sw_started_us);
    if (sw.laps.size() < 99) sw.laps.push_back(el);
    app::mark(app::kDirtyTimer);
}

void stopwatch_reset() {
    std::lock_guard<std::mutex> g(mu);
    sw = StopwatchInfo{};
    sw_accum_us = 0;
    app::mark(app::kDirtyTimer);
}

std::vector<core::Alarm> alarms() {
    std::lock_guard<std::mutex> g(mu);
    return alarm_list;
}

void alarm_upsert(const core::Alarm& a) {
    std::lock_guard<std::mutex> g(mu);
    core::Alarm x = a;
    if (x.id == 0) {
        uint8_t id = 1;
        while (std::any_of(alarm_list.begin(), alarm_list.end(), [&](const core::Alarm& e) { return e.id == id; })) ++id;
        x.id = id;
        if (alarm_list.size() < 16) alarm_list.push_back(x);
    } else {
        for (auto& e : alarm_list)
            if (e.id == x.id) e = x;
    }
    settings::set_alarms(alarm_list);
    app::mark(app::kDirtyAlarms);
}

void alarm_delete(uint8_t id) {
    std::lock_guard<std::mutex> g(mu);
    alarm_list.erase(std::remove_if(alarm_list.begin(), alarm_list.end(), [&](const core::Alarm& e) { return e.id == id; }), alarm_list.end());
    settings::set_alarms(alarm_list);
    app::mark(app::kDirtyAlarms);
}

void alarm_snooze(uint8_t id, int minutes) {
    if (!time_valid()) return;
    std::lock_guard<std::mutex> g(mu);
    snoozes.push_back({id, time(nullptr) + minutes * 60});
}

void notify_feedback(uint8_t pulses, bool audio_tone) {
    auto s = settings::get();
    if (s.vibration_enabled) board::vibrate(180, pulses, 150);
    if (audio_tone) swlink::send_alert_tone();
}

void notifications_changed() {
    int64_t expected = 0;
    notif_dirty_at.compare_exchange_strong(expected, esp_timer_get_time());
}

}  // namespace services
