// Board implementation: Waveshare ESP32-S3-Touch-AMOLED-2.06 (USB powered prototype).
#include <algorithm>
#include <atomic>

#include "axp2101.hpp"
#include "board.hpp"
#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_lcd_touch.h"  // before bsp/touch.h and the port header, which need its types
#include "bsp/touch.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
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

// The panel wants even-aligned areas (same rounding as the BSP's own callback).
void rounder_cb(lv_event_t* e) {
    auto* a = static_cast<lv_area_t*>(lv_event_get_param(e));
    a->x1 &= ~1;
    a->y1 &= ~1;
    a->x2 |= 1;
    a->y2 |= 1;
}

// Frame transfer. LVGL renders whole frames into two PSRAM frame buffers (direct mode), so the
// panel never shows a half-drawn frame the way 50-line partial rendering did (bands appearing
// one after another = tearing while scrolling). A finished frame's changed rows are sent by
// `tx_task` through two small internal DMA bounce buffers (the SPI DMA can't take this PSRAM
// buffer as is), byte-swapped for the panel on the way, while LVGL already renders the next
// frame into the other buffer.
constexpr int kW = BSP_LCD_H_RES, kH = BSP_LCD_V_RES;
constexpr int kChunkRows = 24;  // even (panel needs even-aligned windows); 2 x 19.7 KB internal DMA RAM
esp_lcd_panel_handle_t panel = nullptr;
// esp_lcd's SPI panel IO is not thread-safe (each call collects the previous call's queued
// transfers); tx_task and brightness commands from other tasks share it under this lock.
SemaphoreHandle_t panel_mutex = nullptr;
uint32_t* bounce[2] = {};
uint32_t chunk_seq = 0;  // alternates bounce buffers across frames
TaskHandle_t tx_task_handle = nullptr;
struct TxJob {
    lv_display_t* disp;
    const uint16_t* frame;
    int y1, y2;  // inclusive rows
};
QueueHandle_t tx_queue = nullptr;
int dirty_y1 = kH, dirty_y2 = -1;

void tx_task(void*) {
    TxJob job;
    for (;;) {
        if (xQueueReceive(tx_queue, &job, portMAX_DELAY) != pdTRUE) continue;
        xSemaphoreTake(panel_mutex, portMAX_DELAY);
        for (int y = job.y1; y <= job.y2; y += kChunkRows) {
            int rows = std::min(kChunkRows, job.y2 + 1 - y);
            uint32_t* dst = bounce[chunk_seq++ & 1];
            // The previous draw_bitmap waited for the chunk before it, so this buffer is free.
            const uint32_t* src = reinterpret_cast<const uint32_t*>(job.frame + y * kW);
            for (int i = 0, n = rows * kW / 2; i < n; ++i) {
                uint32_t v = src[i];
                dst[i] = ((v & 0x00FF00FFu) << 8) | ((v >> 8) & 0x00FF00FFu);  // RGB565 byte swap, two pixels
            }
            esp_lcd_panel_draw_bitmap(panel, 0, y, kW, y + rows, dst);
        }
        xSemaphoreGive(panel_mutex);
        lv_display_flush_ready(job.disp);
    }
}

void flush_cb(lv_display_t* d, const lv_area_t* a, uint8_t* px) {
    dirty_y1 = std::min<int>(dirty_y1, a->y1);
    dirty_y2 = std::max<int>(dirty_y2, a->y2);
    if (!lv_display_flush_is_last(d)) {
        lv_display_flush_ready(d);
        return;
    }
    TxJob job{d, reinterpret_cast<const uint16_t*>(px), std::max(0, dirty_y1 & ~1), std::min(kH - 1, dirty_y2 | 1)};
    dirty_y1 = kH;
    dirty_y2 = -1;
    if (job.y2 < job.y1 || xQueueSend(tx_queue, &job, 0) != pdTRUE) lv_display_flush_ready(d);
}

// Chunk completion is tracked by draw_bitmap itself; the port's per-transfer callback would
// mark the whole frame done after the first chunk.
bool on_color_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) { return false; }

// Replaces bsp_display_start_with_config(), which ignores the buffer settings it is given and
// puts a 100-line draw buffer in PSRAM; the SPI driver then malloc'd an ~80 KB internal DMA
// copy per flush, which failed once internal RAM fragmented (stuck bands of old map tiles).
lv_display_t* start_display() {
    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 4;  // below BLE host (prio ~19) so radio work is never starved
    port.task_stack = 12288;
    port.task_affinity = 1;  // UI on core 1, Bluetooth controller/host on core 0
    port.timer_period_ms = 10;
    if (lvgl_port_init(&port) != ESP_OK) return nullptr;

    bsp_display_config_t dcfg = {};
    dcfg.max_transfer_sz = kW * kChunkRows * 2;
    esp_lcd_panel_io_handle_t io = nullptr;
    if (bsp_display_new(&dcfg, &panel, &io) != ESP_OK) return nullptr;
    for (auto& b : bounce) {
        b = static_cast<uint32_t*>(heap_caps_aligned_alloc(4, kW * kChunkRows * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
        if (!b) return nullptr;
    }
    tx_queue = xQueueCreate(1, sizeof(TxJob));
    panel_mutex = xSemaphoreCreateMutex();
    if (!panel_mutex) return nullptr;
    // Core 0, so copying overlaps LVGL rendering on core 1; below the BLE host.
    if (!tx_queue || xTaskCreatePinnedToCore(tx_task, "lcd_tx", 3072, nullptr, 5, &tx_task_handle, 0) != pdPASS) return nullptr;

    lvgl_port_display_cfg_t cfg = {};
    cfg.io_handle = io;
    cfg.panel_handle = panel;
    cfg.buffer_size = kW * kH;  // two full frames in PSRAM (2 x 402 KB)
    cfg.double_buffer = true;
    cfg.hres = kW;
    cfg.vres = kH;
    cfg.color_format = LV_COLOR_FORMAT_RGB565;
    cfg.flags.buff_dma = false;
    cfg.flags.buff_spiram = true;
    cfg.flags.direct_mode = true;
    // No sw_rotate (the UI never rotates; the port would allocate a rotation buffer) and no
    // swap_bytes (tx_task swaps). The port sets the panel orientation instead; sh8601 logs
    // "swap_xy is not supported" and esp_lcd warns that our on_color_trans_done replaced the
    // port's; both harmless.
    // Locked so the LVGL task can't render a frame through the port's own flush callback
    // before ours is installed (it would wait forever for the callback replaced below).
    lvgl_port_lock(0);
    lv_display_t* d = lvgl_port_add_disp(&cfg);
    if (!d) {
        lvgl_port_unlock();
        return nullptr;
    }
    esp_lcd_panel_io_callbacks_t cbs = {};
    cbs.on_color_trans_done = on_color_done;
    esp_lcd_panel_io_register_event_callbacks(io, &cbs, nullptr);
    lv_display_set_flush_cb(d, flush_cb);
    lv_display_add_event_cb(d, rounder_cb, LV_EVENT_INVALIDATE_AREA, nullptr);
    lvgl_port_unlock();

    esp_lcd_touch_handle_t tp = nullptr;
    if (bsp_touch_new(nullptr, &tp) == ESP_OK && tp) {
        lvgl_port_touch_cfg_t tcfg = {};
        tcfg.disp = d;
        tcfg.handle = tp;
        lvgl_port_add_touch(&tcfg);
    } else {
        ESP_LOGE(TAG, "touch init failed");
    }
    xSemaphoreTake(panel_mutex, portMAX_DELAY);
    bsp_display_brightness_init();
    xSemaphoreGive(panel_mutex);
    return d;
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

    disp = start_display();
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
    if (!panel_mutex) return;
    xSemaphoreTake(panel_mutex, portMAX_DELAY);
    bsp_display_brightness_set(percent);
    xSemaphoreGive(panel_mutex);
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
