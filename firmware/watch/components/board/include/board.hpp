// Hardware abstraction used by the watch application.
//
// Everything board-specific (pins, buses, PMIC, sensors, display controller) is behind this
// interface. The app must not include BSP headers directly.
#pragma once

#include <cstdint>

#include "esp_err.h"
#include "lvgl.h"

namespace board {

struct ImuSample {
    float ax, ay, az;  // g
    float gx, gy, gz;  // degrees per second
    int64_t t_us;      // monotonic timestamp
};

struct PowerStatus {
    bool pmic_ok = false;
    bool battery_present = false;
    int percent = -1;  // -1 when unknown / no battery
    bool charging = false;
    bool vbus = false;
    int vbat_mv = 0;
};

struct Capabilities {
    bool imu_accel = false;
    bool imu_gyro = false;
    bool magnetometer = false;  // TODO(HARDWARE): the custom watch's 9-axis IMU provides this.
    bool vibration = false;
    bool battery_gauge = false;
};

// Brings up I2C, PMIC, display + LVGL port, touch, IMU and haptics. Returns ESP_OK if the
// display came up; missing optional peripherals are reported through capabilities().
esp_err_t init();

const char* name();
const Capabilities& capabilities();

lv_display_t* display();
int display_width();
int display_height();
// LVGL is not thread-safe: hold this lock for any LVGL call outside the LVGL task.
bool lvgl_lock(uint32_t timeout_ms = 0);
void lvgl_unlock();
void set_brightness(int percent);

// Reads all IMU axes. Returns false if the IMU is unavailable or the read failed.
bool imu_read(ImuSample& out);

PowerStatus power();

// Non-blocking: drives the motor for `on_ms`, `count` times with `off_ms` gaps.
void vibrate(uint16_t on_ms, uint8_t count = 1, uint16_t off_ms = 120);

}  // namespace board
