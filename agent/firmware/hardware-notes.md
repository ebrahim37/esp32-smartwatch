# Hardware notes (agent-facing research log)

## Waveshare ESP32-S3-Touch-AMOLED-2.06
- Repo: github.com/waveshareteam/ESP32-S3-Touch-AMOLED-2.06. README lists supported IDF: v5.5.5 and v6.0.2.
- `examples/esp-idf/02_lvgl_demo_v9`: deps `waveshare/esp32_s3_touch_amoled_2_06 ^2.0.0`, `lvgl/lvgl 9.5.0`. sdkconfig: QIO 16 MB, octal PSRAM 80 MHz.
- BSP 2.0.0 (registry): CO5300 via `waveshare/esp_lcd_sh8601`, touch via `esp_lcd_touch_ft5x06`, `esp_lvgl_port ^2`. API: `bsp_display_start_with_config`, `bsp_display_brightness_set` (panel command, 0–100), `bsp_i2c_get_handle`. I2C SDA15/SCL14. `BSP_CAPS_IMU 0` → own IMU init via `waveshare/qmi8658 2.0.2` (`qmi8658_read_sensor_data`, accel in mg by default).
- Vibration motor: GPIO18 (factory `VibrationTest.hpp`). BOOT and PWR buttons exist (PWR via inverter, long hold powers off).
- AXP2101 0x34: STATUS1 bit5 VBUS good, bit3 battery present; STATUS2 bits7:5 = 001 charging; 0xA4 percent; 0x18 bit3 gauge enable; 0x30 bit0 VBAT ADC (from XPowersLib).
- Pitfall: `CONFIG_LV_ATTRIBUTE_FAST_MEM_USE_IRAM=y` + NimBLE overflows the 16 KB S3 IRAM ("dangerous relocation: j: cannot encode") → disabled.
- Pitfall: LVGL custom allocator symbols need `target_link_libraries(lvgl_lib PUBLIC board_lib)`.

## Freenove FNK0060
- Repo: github.com/Freenove/Freenove_ESP32_WROVER_Board; `C/Sketches/Sketch_03.1_SDMMC_Test`: `SD_MMC.setPins(14, 15, 2)`, `SD_MMC.begin("/sdcard", true /*1-bit*/ ...)` → confirmed 1-bit, CLK14/CMD15/D0=GPIO2 (slot 1 IOMUX pins).
- ESP32 maps only 4 MB of the 8 MB PSRAM directly (`SPI DRAM 4194304` in size report).
- 4 MB flash: dual-mode Bluedroid + Wi-Fi + TLS + esp_audio_codec (MP3/AAC/FLAC/ALAC/M4A) ≈ 1.9 MB app; IRAM 97 % used (watch this when adding features).

## Toolchain
- nixpkgs-esp-dev pinned 5287d6e1; overridden to ESP-IDF v5.5.5 (hash in flake). xtensa-esp-elf-gdb dropped (auto-patchelf fails on python3.14 variant).
- idf-component-manager 2.2.2 (bundled) aborts with `MissingKconfigError: LV_USE_FREETYPE` while solving lvgl 9.6 deps; 2.4.9 added to PYTHONPATH in the flake fixes it.
