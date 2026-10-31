#include <cmath>
#include <cstdlib>

#include "core/alarm_schedule.hpp"
#include "core/calculator.hpp"
#include "core/swt.hpp"
#include "core/format.hpp"
#include "core/heading.hpp"
#include "core/map_math.hpp"
#include "core/notification_store.hpp"
#include "core/step_counter.hpp"
#include "minitest.hpp"

using namespace core;

static void use_tz(const char* tz) {
    setenv("TZ", tz, 1);
    tzset();
}

// Synthetic walking: gravity on a tilted axis plus a 0.35 g sinusoid at `hz` steps/s.
static uint32_t simulate_walk(StepCounter& sc, double seconds, double hz, double amp_g, int64_t& t) {
    uint32_t steps = 0;
    for (int i = 0; i < static_cast<int>(seconds * 50); ++i) {
        double ph = 2 * M_PI * hz * (t / 1e6);
        float dyn = static_cast<float>(amp_g * std::sin(ph));
        // Orientation: gravity split across x/z, dynamic part along the same direction.
        float g = 1.0f + dyn;
        steps += sc.add_sample(0.6f * g, 0.0f, 0.8f * g, t);
        t += 20000;
    }
    return steps;
}

TEST(steps_counts_regular_walking) {
    StepCounter sc;
    int64_t t = 1000000;
    uint32_t n = simulate_walk(sc, 30, 1.8, 0.35, t);
    CHECK(n >= 50 && n <= 56);  // 54 cycles expected, minus filter warm-up
}

TEST(steps_ignores_stillness_and_small_motion) {
    StepCounter sc;
    int64_t t = 1000000;
    CHECK_EQ(simulate_walk(sc, 20, 1.8, 0.03, t), 0u);
}

TEST(steps_rejects_isolated_movements) {
    StepCounter sc;
    int64_t t = 1000000;
    // Three isolated jolts separated by long pauses never pass the regularity gate.
    uint32_t n = 0;
    for (int k = 0; k < 3; ++k) {
        n += simulate_walk(sc, 0.6, 1.8, 0.5, t);
        n += simulate_walk(sc, 4, 1.8, 0.0, t);
    }
    CHECK_EQ(n, 0u);
}

TEST(steps_threshold_configurable) {
    StepConfig cfg;
    cfg.threshold_g = 0.6f;
    StepCounter sc(cfg);
    int64_t t = 1000000;
    CHECK_EQ(simulate_walk(sc, 10, 1.8, 0.35, t), 0u);
}

TEST(heading_integrates_yaw_any_orientation) {
    // Device tilted so gravity is along +x; rotate at 30 dps about that axis for 3 s.
    RelativeHeading h;
    int64_t t = 0;
    for (int i = 0; i <= 150; ++i) {
        h.update(1.0f, 0, 0, 30.0f, 0, 0, t);
        t += 20000;
    }
    // CCW about up => heading decreases: 0 - 90 = 270.
    CHECK(std::fabs(h.degrees() - 270.f) < 3.f);
    CHECK(!h.is_magnetic());
}

TEST(heading_reset_to_north) {
    RelativeHeading h;
    h.reset();
    CHECK_EQ(h.degrees(), 0.f);
}

TEST(calculator_clear_entry_keeps_pending_op) {
    Calculator c;
    CHECK(!c.has_entry());
    c.digit(8); c.op(Calculator::Op::Mul); CHECK(!c.has_entry());
    c.digit(7); CHECK(c.has_entry());
    c.clear_entry(); CHECK(!c.has_entry()); CHECK_EQ(c.display(), std::string("0"));
    c.digit(2); c.equals(); CHECK_EQ(c.display(), std::string("16"));
    c.digit(1); c.op(Calculator::Op::Div); c.digit(0); c.equals();
    CHECK(c.has_entry());  // error: C resets
    c.clear_entry(); CHECK_EQ(c.display(), std::string("0"));
}

TEST(calculator_precedence_and_ops) {
    Calculator c;
    c.digit(2); c.op(Calculator::Op::Add); c.digit(3); c.op(Calculator::Op::Mul); c.digit(4); c.equals();
    CHECK_EQ(c.display(), std::string("14"));
    c.clear();
    c.digit(1); c.op(Calculator::Op::Div); c.digit(0); c.equals();
    CHECK(c.error());
    c.clear();
    c.digit(5); c.decimal_point(); c.digit(5); c.toggle_sign();
    CHECK_EQ(c.display(), std::string("-5.5"));
    c.clear();
    c.digit(5); c.digit(0); c.op(Calculator::Op::Add); c.digit(1); c.digit(0); c.percent(); c.equals();
    CHECK_EQ(c.display(), std::string("55"));
    c.clear();
    c.digit(1); c.op(Calculator::Op::Div); c.digit(3); c.equals();
    CHECK_EQ(c.display(), std::string("0.333333333"));
}

TEST(alarm_fires_once_in_window) {
    use_tz("UTC0");
    std::vector<Alarm> alarms(1);
    alarms[0].id = 1;
    alarms[0].hour = 7;
    alarms[0].minute = 30;
    alarms[0].days = 0;  // one-shot
    AlarmScheduler s;
    time_t base = 1761800000;  // 2025-10-30 04:53:20 UTC
    struct tm lt;
    gmtime_r(&base, &lt);
    lt.tm_hour = 7; lt.tm_min = 29; lt.tm_sec = 50;
    time_t t0 = timegm(&lt);
    CHECK(s.poll(alarms, t0).empty());  // anchors
    CHECK(s.poll(alarms, t0 + 5).empty());
    auto fired = s.poll(alarms, t0 + 15);
    CHECK_EQ(fired.size(), 1u);
    CHECK(!alarms[0].enabled);
    CHECK(s.poll(alarms, t0 + 30).empty());
}

TEST(alarm_skips_after_clock_jump_and_invalid_time) {
    use_tz("UTC0");
    std::vector<Alarm> alarms(1);
    alarms[0].hour = 7;
    alarms[0].days = 0x7F;
    AlarmScheduler s;
    CHECK(s.poll(alarms, 1000).empty());  // unsynced clock
    time_t t = 1761800000;
    s.poll(alarms, t);
    CHECK(s.poll(alarms, t + 6 * 3600).empty());  // jump over 07:00 -> no late burst
}

TEST(alarm_weekday_mask_and_dst) {
    use_tz("EST5EDT,M3.2.0,M11.1.0");
    Alarm a;
    a.hour = 6;
    a.minute = 0;
    a.days = 0x3E;  // weekdays
    // Fri 2026-10-30 12:00 EDT -> next is Mon 2026-11-02 06:00 EST (after DST ends).
    struct tm lt = {};
    lt.tm_year = 126; lt.tm_mon = 9; lt.tm_mday = 30; lt.tm_hour = 12; lt.tm_isdst = -1;
    time_t from = mktime(&lt);
    time_t nt = next_trigger(a, from);
    struct tm r;
    localtime_r(&nt, &r);
    CHECK_EQ(r.tm_wday, 1);
    CHECK_EQ(r.tm_mday, 2);
    CHECK_EQ(r.tm_hour, 6);
    CHECK_EQ(days_summary(0x3E), std::string("Weekdays"));
    CHECK_EQ(days_summary(0x0A), std::string("Mon Wed"));
}

TEST(format_helpers) {
    CHECK_EQ(fmt_elapsed_remaining(142000, 147000), std::string("2:22 / -0:05"));
    CHECK_EQ(fmt_duration(3725), std::string("1:02:05"));
    CHECK_EQ(fmt_stopwatch(61230000), std::string("01:01.23"));
    struct tm lt = {};
    lt.tm_wday = 5; lt.tm_mon = 9; lt.tm_mday = 30; lt.tm_hour = 21; lt.tm_min = 8;
    CHECK_EQ(fmt_date_short(lt), std::string("Fri Oct 30"));
    CHECK_EQ(fmt_clock(lt, false), std::string("9:08"));
    CHECK_EQ(fmt_precip(0, -1, 24), std::string("no precipitation"));
    CHECK_EQ(fmt_precip(1, 240, 24), std::string("rain in 4 hrs"));
    CHECK_EQ(fmt_precip(2, 20, 24), std::string("snow in 20 min"));
    CHECK_EQ(fmt_precip(1, 0, 24), std::string("rain now"));
    CHECK(uv_level(6) == Level::High);
    CHECK(battery_level(33) == Level::Moderate);
    CHECK_EQ(fmt_temp(-35), std::string("-4"));
    CHECK_EQ(fmt_temp(104), std::string("10"));
    CHECK_EQ(std::string(wmo_description(63)), std::string("raining"));
}

TEST(notification_dedup_and_persistence) {
    NotificationStore s(3);
    StoredNotification n;
    for (uint64_t i = 1; i <= 4; ++i) {
        n.id = i;
        n.title = "t" + std::to_string(i);
        CHECK(s.add(n));
    }
    CHECK(!s.add(n));  // redelivery of id 4
    CHECK_EQ(s.items().size(), 3u);
    CHECK_EQ(s.items().front().id, 4u);
    CHECK(s.dismiss(3));
    auto blob = s.serialize();
    NotificationStore s2(3);
    CHECK(s2.deserialize(blob));
    CHECK_EQ(s2.items().size(), 2u);
    n.id = 2;
    CHECK(!s2.add(n));  // seen-set persisted: dismissed items are not resurrected
    blob.resize(blob.size() - 3);
    NotificationStore s3;
    CHECK(!s3.deserialize(blob));
}

TEST(map_math_roundtrip) {
    double lat = 43 + 46.0 / 60 + 48.3 / 3600, lon = -(79 + 24.0 / 60 + 42.7 / 3600);
    auto p = latlon_to_px(lat, lon, 15);
    double la, lo;
    px_to_latlon(p, 15, la, lo);
    CHECK(std::fabs(la - lat) < 1e-7 && std::fabs(lo - lon) < 1e-7);
    CHECK_EQ(static_cast<int>(p.x / 256), 9155);  // (lon+180)/360 * 2^15 = 9155.75
    CHECK(std::fabs(haversine_m(43.0, -79.0, 43.01, -79.0) - 1112) < 5);
}

TEST(swt_header_and_palette_expansion) {
    using namespace core;
    uint8_t t[10 + 48 + 4] = {'S', 'W', 'T', '1', 0, 1, 0, 1, 1, 16};
    t[10] = 0xFF; t[11] = 0x00; t[12] = 0x00;  // colour 0: red
    t[13] = 0x00; t[14] = 0x00; t[15] = 0xFF;  // colour 1: blue
    SwtHeader h;
    CHECK(parse_swt(t, sizeof t, h));
    CHECK_EQ(h.payload_offset, size_t(58));
    CHECK_EQ(h.palette565[0], uint16_t(0xF800));
    CHECK_EQ(h.palette565[1], uint16_t(0x001F));
    t[3] = '2';
    CHECK(!parse_swt(t, sizeof t, h));
    t[3] = '1'; t[5] = 0;  // width 0
    CHECK(!parse_swt(t, sizeof t, h));
    const uint8_t idx[2] = {0x01, 0x10};
    uint16_t px[4];
    uint16_t pal[16] = {0xF800, 0x001F};
    expand_i4(idx, pal, px, 4);
    CHECK(px[0] == 0xF800 && px[1] == 0x001F && px[2] == 0x001F && px[3] == 0xF800);
}

int main() { return minitest::run_all(); }
