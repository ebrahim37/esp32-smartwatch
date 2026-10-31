// Background services that run independently of the visible screen: IMU (steps + relative
// heading), wall clock, timer, stopwatch, alarms, power telemetry.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "board.hpp"
#include "core/alarm_schedule.hpp"

namespace services {

void start();

// ---- Time ------------------------------------------------------------------------------
bool time_valid();
void apply_time_sync(uint64_t utc_ms);
bool local_now(struct tm& out);  // false if the wall clock has never been synchronised

// ---- Sensors ---------------------------------------------------------------------------
uint32_t steps_today();
float heading_deg();  // TODO(PROTOTYPE): relative gyro heading, not magnetic
void reset_heading();
void set_step_threshold_mg(uint16_t mg);

// ---- Power -----------------------------------------------------------------------------
board::PowerStatus power();

// ---- Timer -----------------------------------------------------------------------------
enum class TimerState : uint8_t { Idle, Running, Paused, Done };
struct TimerInfo {
    TimerState state = TimerState::Idle;
    uint32_t duration_s = 600;
    int64_t remaining_ms = 600000;
};
TimerInfo timer();
void timer_set(uint32_t seconds);
void timer_start();
void timer_pause();
void timer_reset();

// ---- Stopwatch -------------------------------------------------------------------------
struct StopwatchInfo {
    bool running = false;
    int64_t elapsed_us = 0;
    std::vector<int64_t> laps;  // lap split times (us), newest last
};
StopwatchInfo stopwatch();
void stopwatch_toggle();
void stopwatch_lap();
void stopwatch_reset();

// ---- Alarms ----------------------------------------------------------------------------
std::vector<core::Alarm> alarms();
void alarm_upsert(const core::Alarm& a);  // id 0 = new
void alarm_delete(uint8_t id);
void alarm_snooze(uint8_t id, int minutes);

// ---- Feedback --------------------------------------------------------------------------
// Vibrates (if enabled in settings) and, if requested and the companion has a connected
// audio sink, asks it to play an alert tone.
void notify_feedback(uint8_t pattern_pulses, bool audio_tone);

void notifications_changed();  // schedules a debounced persistence of history

}  // namespace services
