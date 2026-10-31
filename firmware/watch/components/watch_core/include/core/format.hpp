// Text formatting shared by watch screens. Pure functions for host testing.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>

namespace core {

// "m:ss" (or "h:mm:ss" past an hour). Negative values are clamped to 0.
std::string fmt_duration(int64_t seconds);
// Player time readout: "2:22 / -0:05".
std::string fmt_elapsed_remaining(uint32_t pos_ms, uint32_t dur_ms);
// Stopwatch "mm:ss.cc".
std::string fmt_stopwatch(int64_t us);
// "Fri Oct 30" from local time.
std::string fmt_date_short(const struct tm& lt);
// "9:08" (12 h, no leading zero) or "09:08" (24 h).
std::string fmt_clock(const struct tm& lt, bool h24);

// Precipitation outlook from minutes-until value: "no precipitation", "rain now",
// "rain in 25 min", "snow in 4 hrs", "precip. unknown".
std::string fmt_precip(uint8_t kind, int32_t minutes, uint8_t horizon_h);

// Level buckets used to colour home-screen values.
enum class Level : uint8_t { Good, Moderate, High, Severe, Unknown };
Level uv_level(float uv);        // WHO UV index bands
Level battery_level(int percent); // >50 good, >20 moderate, >10 high, else severe

// Short condition text for a WMO weather code ("clear", "raining", ...).
const char* wmo_description(uint8_t code);

// Temperature in tenths of a degree to rounded integer string ("-3").
std::string fmt_temp(int16_t c10);

}  // namespace core
