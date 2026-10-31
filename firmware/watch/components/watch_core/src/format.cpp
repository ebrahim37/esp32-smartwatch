#include "core/format.hpp"

#include <cstdio>

namespace core {

std::string fmt_duration(int64_t s) {
    if (s < 0) s = 0;
    char b[24];
    if (s >= 3600)
        std::snprintf(b, sizeof b, "%d:%02d:%02d", static_cast<int>(s / 3600), static_cast<int>(s / 60 % 60), static_cast<int>(s % 60));
    else
        std::snprintf(b, sizeof b, "%d:%02d", static_cast<int>(s / 60), static_cast<int>(s % 60));
    return b;
}

std::string fmt_elapsed_remaining(uint32_t pos_ms, uint32_t dur_ms) {
    if (pos_ms > dur_ms && dur_ms > 0) pos_ms = dur_ms;
    int64_t pos = pos_ms / 1000;
    int64_t rem = dur_ms > 0 ? (static_cast<int64_t>(dur_ms) - pos_ms + 999) / 1000 : 0;
    return fmt_duration(pos) + " / -" + fmt_duration(rem);
}

std::string fmt_stopwatch(int64_t us) {
    if (us < 0) us = 0;
    int64_t cs = us / 10000;
    char b[24];
    int64_t m = cs / 6000;
    if (m >= 60)
        std::snprintf(b, sizeof b, "%d:%02d:%02d.%02d", static_cast<int>(m / 60), static_cast<int>(m % 60), static_cast<int>(cs / 100 % 60), static_cast<int>(cs % 100));
    else
        std::snprintf(b, sizeof b, "%02d:%02d.%02d", static_cast<int>(m), static_cast<int>(cs / 100 % 60), static_cast<int>(cs % 100));
    return b;
}

std::string fmt_date_short(const struct tm& lt) {
    static const char* wd[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char* mo[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    char b[24];
    std::snprintf(b, sizeof b, "%s %s %d", wd[lt.tm_wday % 7], mo[lt.tm_mon % 12], lt.tm_mday);
    return b;
}

std::string fmt_clock(const struct tm& lt, bool h24) {
    char b[8];
    if (h24) {
        std::snprintf(b, sizeof b, "%02d:%02d", lt.tm_hour, lt.tm_min);
    } else {
        int h = lt.tm_hour % 12;
        std::snprintf(b, sizeof b, "%d:%02d", h == 0 ? 12 : h, lt.tm_min);
    }
    return b;
}

std::string fmt_precip(uint8_t kind, int32_t minutes, uint8_t horizon_h) {
    const char* what = kind == 2 ? "snow" : kind == 3 ? "sleet" : "rain";
    if (kind == 0xFF) return "precip. unknown";
    if (kind == 0 || minutes < 0) {
        (void)horizon_h;
        return "no precipitation";
    }
    char b[40];
    if (minutes == 0)
        std::snprintf(b, sizeof b, "%s now", what);
    else if (minutes < 60)
        std::snprintf(b, sizeof b, "%s in %d min", what, static_cast<int>(minutes));
    else {
        int h = static_cast<int>((minutes + 30) / 60);
        std::snprintf(b, sizeof b, "%s in %d hr%s", what, h, h == 1 ? "" : "s");
    }
    return b;
}

Level uv_level(float uv) {
    if (uv < 0) return Level::Unknown;
    if (uv < 3) return Level::Good;
    if (uv < 6) return Level::Moderate;
    if (uv < 8) return Level::High;
    return Level::Severe;
}

Level battery_level(int p) {
    if (p < 0) return Level::Unknown;
    if (p > 50) return Level::Good;
    if (p > 20) return Level::Moderate;
    if (p > 10) return Level::High;
    return Level::Severe;
}

const char* wmo_description(uint8_t c) {
    switch (c) {
        case 0: return "clear";
        case 1: return "mostly clear";
        case 2: return "partly cloudy";
        case 3: return "overcast";
        case 45: case 48: return "foggy";
        case 51: case 53: case 55: return "drizzle";
        case 56: case 57: return "freezing drizzle";
        case 61: case 63: return "raining";
        case 65: return "heavy rain";
        case 66: case 67: return "freezing rain";
        case 71: case 73: return "snowing";
        case 75: return "heavy snow";
        case 77: return "snow grains";
        case 80: case 81: return "showers";
        case 82: return "heavy showers";
        case 85: case 86: return "snow showers";
        case 95: return "thunderstorm";
        case 96: case 99: return "hail storm";
        default: return "unknown";
    }
}

std::string fmt_temp(int16_t c10) {
    int v = c10 >= 0 ? (c10 + 5) / 10 : -((-c10 + 5) / 10);
    return std::to_string(v);
}

}  // namespace core
