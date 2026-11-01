# Hardware

## Watch prototype — Waveshare ESP32-S3-Touch-AMOLED-2.06

| Function | Detail |
|----------|--------|
| SoC | ESP32-S3R8, 8 MB octal PSRAM, 32 MB flash (configured as 16 MB, see below) |
| Display | 2.06" 410×502 AMOLED, CO5300 over QSPI (CS 12, PCLK 11, D0–D3 4–7, RST 8) — driven by Waveshare's BSP (`esp_lcd_sh8601` compatible driver) |
| Touch | FT3168 on I2C (INT 38, RST 9), FT5x06-compatible driver |
| I2C | SDA 15, SCL 14 (BSP bus, 400 kHz) shared by touch, AXP2101 (0x34), QMI8658 (0x6B/0x6A) |
| IMU | QMI8658 6-axis; sampled at 50 Hz (125 Hz ODR, ±4 g, ±512 dps) |
| PMIC | AXP2101: status, fuel gauge, battery ADC. Prototype runs from USB with no cell → UI shows `BAT: USB` |
| Haptics | Motor driver on GPIO18 (from Waveshare's factory VibrationTest) |
| Unused on purpose | SD slot, ES8311/ES7210 audio, PCF85063 RTC (the future watch has none of these) |

Flash: Waveshare's validated configuration and ESP-IDF's stable flash driver address
16 MB; >16 MB needs experimental 32-bit addressing. `TODO(HARDWARE)` in
`firmware/watch/sdkconfig.defaults`.

## Companion prototype — Freenove FNK0060 (ESP32-WROVER-E)

| Function | Detail |
|----------|--------|
| SoC | ESP32 (dual-core, BR/EDR + BLE, Wi-Fi), 4 MB flash, 8 MB PSRAM (4 MB directly mapped by ESP32) |
| microSD | **1-bit SDMMC, slot 1: CLK GPIO14, CMD GPIO15, D0 GPIO2** — verified against Freenove's `Sketch_03.1_SDMMC_Test` ("SD_MMC.setPins(14, 15, 2)", one-bit mode) |
| Buttons | BOOT on GPIO0 (unused by the firmware) |
| USB | CH340 serial (`/dev/ttyUSB0`) |
| Camera | present, unused |

GPIO2 is a strapping pin shared with SD D0; leave the card inserted or not — both boot
normally on this board, but if auto-download fails, remove the card while flashing.
