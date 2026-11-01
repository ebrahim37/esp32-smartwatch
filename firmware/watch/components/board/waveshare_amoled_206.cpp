// Board implementation: Waveshare ESP32-S3-Touch-AMOLED-2.06 (USB powered prototype).
#include <atomic>

#include "axp2101.hpp"
#include "board.hpp"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "qmi8658.h"
#include "src/display/lv_display_private.h"  // `flushing`, for the flush watchdog

namespace board {
namespace {

constexpr const char* TAG = "board";
constexpr gpio_num_t kMotorGpio = GPIO_NUM_18;  // vibration motor driver (factory VibrationTest)

Capabilities caps;
lv_display_t* disp = nullptr;
qmi8658_dev_t imu_dev;
SemaphoreHandle_t i2c_mutex = nullptr;  // serializes our IMU/PMIC reads across tasks

// Flush watchdog: LVGL busy-waits until the panel driver reports a flush done. If that
// report is ever lost the UI freezes mid-frame (horizontal bands of a half-drawn screen).
esp_timer_handle_t flush_wd = nullptr;
int64_t flushing_since_us = 0;
std::atomic<bool> recovered{false};

void flush_wd_cb(void*) {
    if (!disp || !disp->flushing) {
        flushing_since_us = 0;
        return;
    }
    int64_t now = esp_timer_get_time();
    if (!flushing_since_us) flushing_since_us = now;
    if (now - flushing_since_us > 500 * 1000) {
        ESP_LOGE(TAG, "display flush not completed after %lld ms; forcing it", (now - flushing_since_us) / 1000);
        flushing_since_us = 0;
        recovered.store(true);
        lv_display_flush_ready(disp);
    }
}

// Haptic pattern state, driven by a one-shot esp_timer so callers never block.
esp_timer_handle_t motor_timer = nullptr;
std::atomic<uint8_t> motor_pulses_left{0};
uint16_t motor_on_ms = 0, motor_off_ms = 0;
bool motor_is_on = false;

void motor_step(void*) {
    if (motor_is_on) {
        gpio_set_level(kMotorGpio, 0);
        motor_is_on = false;
        if (motor_pulses_left.load() > 0) esp_timer_start_once(motor_timer, motor_off_ms * 1000ULL);
        return;
    }
    if (motor_pulses_left.load() == 0) return;
    motor_pulses_left.fetch_sub(1);
    gpio_set_level(kMotorGpio, 1);
    motor_is_on = true;
    esp_timer_start_once(motor_timer, motor_on_ms * 1000ULL);
}

void init_motor() {
    gpio_set_level(kMotorGpio, 0);
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << kMotorGpio;
    io.mode = GPIO_MODE_OUTPUT;
    if (gpio_config(&io) != ESP_OK) return;
    gpio_set_level(kMotorGpio, 0);
    esp_timer_create_args_t args = {};
    args.callback = motor_step;
    args.name = "motor";
    if (esp_timer_create(&args, &motor_timer) == ESP_OK) caps.vibration = true;
}

void init_imu(i2c_master_bus_handle_t bus) {
    uint8_t addr = 0;
    if (i2c_master_probe(bus, QMI8658_ADDRESS_HIGH, 50) == ESP_OK)
        addr = QMI8658_ADDRESS_HIGH;
    else if (i2c_master_probe(bus, QMI8658_ADDRESS_LOW, 50) == ESP_OK)
        addr = QMI8658_ADDRESS_LOW;
    if (!addr || qmi8658_init(&imu_dev, bus, addr) != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 not found");
        return;
    }
    // 4 g / 512 dps at ~125 Hz output: enough for gait and wrist rotation, low power.
    qmi8658_set_accel_range(&imu_dev, QMI8658_ACCEL_RANGE_4G);
    qmi8658_set_accel_odr(&imu_dev, QMI8658_ACCEL_ODR_125HZ);
    qmi8658_set_gyro_range(&imu_dev, QMI8658_GYRO_RANGE_512DPS);
    qmi8658_set_gyro_odr(&imu_dev, QMI8658_GYRO_ODR_125HZ);
    qmi8658_set_accel_unit_mps2(&imu_dev, false);
    qmi8658_set_gyro_unit_rads(&imu_dev, false);
    qmi8658_enable_sensors(&imu_dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO);
    caps.imu_accel = caps.imu_gyro = true;
    ESP_LOGI(TAG, "QMI8658 at 0x%02x", addr);
}

}  // namespace

const char* name() { return "Waveshare ESP32-S3-Touch-AMOLED-2.06"; }
const Capabilities& capabilities() { return caps; }
lv_display_t* display() { return disp; }
int display_width() { return BSP_LCD_H_RES; }
int display_height() { return BSP_LCD_V_RES; }

esp_err_t init() {
    i2c_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(bsp_i2c_init());
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();

    if (axp2101::init(bus) == ESP_OK) caps.battery_gauge = true;

    bsp_display_cfg_t cfg = {};
    cfg.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    cfg.lvgl_port_cfg.task_priority = 4;  // below BLE host (prio ~19) so radio work is never starved
    cfg.lvgl_port_cfg.task_stack = 12288;
    cfg.lvgl_port_cfg.task_affinity = 1;  // UI on core 1, Bluetooth controller/host on core 0
    cfg.lvgl_port_cfg.timer_period_ms = 10;
    cfg.buffer_size = BSP_LCD_H_RES * 50;  // 50-line partial buffer in internal DMA RAM (~40 KB)
    cfg.double_buffer = false;
    cfg.flags.buff_dma = true;
    cfg.flags.buff_spiram = false;
    disp = bsp_display_start_with_config(&cfg);
    if (!disp) return ESP_FAIL;
    esp_timer_create_args_t wd = {};
    wd.callback = flush_wd_cb;
    wd.name = "flush_wd";
    if (esp_timer_create(&wd, &flush_wd) == ESP_OK) esp_timer_start_periodic(flush_wd, 100 * 1000);

    init_imu(bus);
    init_motor();
    // TODO(HARDWARE): the prototype has no battery fitted; PowerStatus reports
    // battery_present=false and the UI shows "BAT: USB". The custom watch adds a cell.
    return ESP_OK;
}

bool lvgl_lock(uint32_t timeout_ms) { return lvgl_port_lock(timeout_ms); }
void lvgl_unlock() { lvgl_port_unlock(); }
bool take_display_recovered() { return recovered.exchange(false); }

void set_brightness(int percent) {
    if (percent < 1) percent = 1;
    if (percent > 100) percent = 100;
    bsp_display_brightness_set(percent);
}

bool imu_read(ImuSample& out) {
    if (!caps.imu_accel) return false;
    qmi8658_data_t d;
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    esp_err_t err = qmi8658_read_sensor_data(&imu_dev, &d);
    xSemaphoreGive(i2c_mutex);
    if (err != ESP_OK) return false;
    // Driver reports mg for accel in mg mode.
    out.ax = d.accelX / 1000.0f;
    out.ay = d.accelY / 1000.0f;
    out.az = d.accelZ / 1000.0f;
    out.gx = d.gyroX;
    out.gy = d.gyroY;
    out.gz = d.gyroZ;
    out.t_us = esp_timer_get_time();
    return true;
}

PowerStatus power() {
    PowerStatus p;
    if (!caps.battery_gauge) return p;
    xSemaphoreTake(i2c_mutex, portMAX_DELAY);
    axp2101::read_status(p);
    xSemaphoreGive(i2c_mutex);
    return p;
}

void vibrate(uint16_t on_ms, uint8_t count, uint16_t off_ms) {
    if (!caps.vibration || count == 0) return;
    esp_timer_stop(motor_timer);
    gpio_set_level(kMotorGpio, 0);
    motor_is_on = false;
    motor_on_ms = on_ms;
    motor_off_ms = off_ms;
    motor_pulses_left.store(count);
    motor_step(nullptr);
}

}  // namespace board
