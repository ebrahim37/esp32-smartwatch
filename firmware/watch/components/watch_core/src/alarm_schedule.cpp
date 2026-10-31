#include "core/alarm_schedule.hpp"

namespace core {

time_t next_trigger(const Alarm& a, time_t after_utc) {
    if (!a.enabled) return 0;
    struct tm lt;
    localtime_r(&after_utc, &lt);
    // Walk forward day by day (max 8) in local time; mktime normalizes DST transitions.
    for (int d = 0; d <= 7; ++d) {
        struct tm cand = lt;
        cand.tm_mday += d;
        cand.tm_hour = a.hour;
        cand.tm_min = a.minute;
        cand.tm_sec = 0;
        cand.tm_isdst = -1;
        time_t t = mktime(&cand);
        if (t <= after_utc) continue;
        struct tm chk;
        localtime_r(&t, &chk);
        if (a.days != 0 && !(a.days & (1u << chk.tm_wday))) continue;
        return t;
    }
    return 0;
}

std::vector<uint8_t> AlarmScheduler::poll(std::vector<Alarm>& alarms, time_t now_utc) {
    std::vector<uint8_t> fired;
    if (now_utc < kMinValidUtc) {
        last_ = 0;
        return fired;
    }
    if (last_ == 0 || now_utc < last_ || now_utc - last_ > kMaxCatchUpS) {
        last_ = now_utc;  // (re)anchor without firing stale alarms
        return fired;
    }
    for (auto& a : alarms) {
        time_t t = next_trigger(a, last_);
        if (t != 0 && t <= now_utc) {
            fired.push_back(a.id);
            if (a.days == 0) a.enabled = false;
        }
    }
    last_ = now_utc;
    return fired;
}

std::string days_summary(uint8_t days) {
    days &= 0x7F;
    if (days == 0) return "Once";
    if (days == 0x7F) return "Every day";
    if (days == 0x3E) return "Weekdays";
    if (days == 0x41) return "Weekends";
    static const char* names[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    std::string s;
    for (int i = 0; i < 7; ++i)
        if (days & (1u << i)) {
            if (!s.empty()) s += ' ';
            s += names[i];
        }
    return s;
}

}  // namespace core
