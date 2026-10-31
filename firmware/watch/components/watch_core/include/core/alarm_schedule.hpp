// Alarm model and wall-clock trigger logic.
//
// Alarms are defined in local time. `AlarmScheduler::poll` is called periodically with the
// current UTC time; an alarm fires when its local trigger minute falls inside the window
// (last_poll, now]. If wall-clock time is invalid nothing fires; if the clock jumps (e.g. the
// first time sync after boot, or a large correction) by more than `kMaxCatchUpS`, missed
// alarms are skipped rather than firing late in a burst.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace core {

struct Alarm {
    uint8_t id = 0;
    uint8_t hour = 7;
    uint8_t minute = 0;
    uint8_t days = 0;  // bit0 = Sunday .. bit6 = Saturday; 0 = one-shot
    bool enabled = true;
    std::string label;
};

// Earliest UTC time > after_utc at which `a` triggers, or 0 if disabled.
time_t next_trigger(const Alarm& a, time_t after_utc);

class AlarmScheduler {
public:
    static constexpr time_t kMaxCatchUpS = 120;
    static constexpr time_t kMinValidUtc = 1704067200;  // 2024-01-01: anything earlier is unsynced

    // Returns ids of alarms that fired. One-shot alarms are disabled in `alarms` after firing.
    std::vector<uint8_t> poll(std::vector<Alarm>& alarms, time_t now_utc);
    void reset() { last_ = 0; }

private:
    time_t last_ = 0;
};

std::string days_summary(uint8_t days);  // "Every day", "Weekdays", "Mon Wed", "Once"

}  // namespace core
