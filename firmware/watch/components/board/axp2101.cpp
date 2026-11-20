#include "axp2101.hpp"

#include <cstdio>

#include "esp_log.h"

namespace board::axp2101 {
namespace {

constexpr const char* TAG = "axp2101";
constexpr uint8_t kAddr = 0x34;
constexpr uint8_t kRegStatus1 = 0x00;  // bit5 VBUS good, bit3 battery present
constexpr uint8_t kRegStatus2 = 0x01;  // bits[7:5] = 001 charging
constexpr uint8_t kRegGaugeCtrl = 0x18;  // bit3 fuel gauge enable
constexpr uint8_t kRegAdcCtrl = 0x30;    // bit0 battery voltage ADC enable
constexpr uint8_t kRegVbatH = 0x34;
constexpr uint8_t kRegBatPercent = 0xA4;
constexpr uint8_t kRegChipId = 0x03;
constexpr uint8_t kRegChargeCurrent = 0x62;
constexpr uint8_t kRegDcdcEnable = 0x80;
constexpr uint8_t kRegLdoEnable = 0x90;  // bit0 ALDO1
constexpr uint8_t kRegAldo1Volts = 0x92;  // bits[4:0], 0.5 V + 0.1 V per step

i2c_master_dev_handle_t dev = nullptr;
char rails_text[32] = "unknown";
char regs_text[32] = "";

bool rd(uint8_t reg, uint8_t* buf, size_t n) {
    return dev && i2c_master_transmit_receive(dev, &reg, 1, buf, n, 50) == ESP_OK;
}

bool wr(uint8_t reg, uint8_t v) {
    uint8_t b[2] = {reg, v};
    return dev && i2c_master_transmit(dev, b, 2, 50) == ESP_OK;
}

bool set_bit(uint8_t reg, int bit) {
    uint8_t v = 0;
    return rd(reg, &v, 1) && wr(reg, static_cast<uint8_t>(v | (1u << bit)));
}

// Must run before anything writes to the PMIC, so the values are what it powered up with.
void capture_boot_state(uint8_t id) {
    uint8_t dcdc = 0, ldo = 0, aldo1 = 0, chg = 0;
    if (!rd(kRegDcdcEnable, &dcdc, 1) || !rd(kRegLdoEnable, &ldo, 1) || !rd(kRegAldo1Volts, &aldo1, 1) || !rd(kRegChargeCurrent, &chg, 1)) return;
    int tenths = 5 + (aldo1 & 0x1F);
    snprintf(rails_text, sizeof(rails_text), "ALDO1 %s %d.%dV", (ldo & 1) ? "ON" : "off", tenths / 10, tenths % 10);
    snprintf(regs_text, sizeof(regs_text), "80:%02X 90:%02X 62:%02X 03:%02X", dcdc, ldo, chg, id);
    ESP_LOGI(TAG, "boot state: %s, %s", rails_text, regs_text);
}

}  // namespace

esp_err_t init(i2c_master_bus_handle_t bus) {
    if (i2c_master_probe(bus, kAddr, 50) != ESP_OK) return ESP_ERR_NOT_FOUND;
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = kAddr;
    cfg.scl_speed_hz = 400000;
    esp_err_t err = i2c_master_bus_add_device(bus, &cfg, &dev);
    if (err != ESP_OK) return err;
    uint8_t id = 0;
    rd(kRegChipId, &id, 1);
    capture_boot_state(id);
    // Rails are left exactly as the bootloader/PMIC defaults configure them (the display
    // and touch are already powered); only measurement features are enabled here.
    set_bit(kRegGaugeCtrl, 3);
    set_bit(kRegAdcCtrl, 0);
    ESP_LOGI(TAG, "AXP2101 found (chip id 0x%02x)", id);
    return ESP_OK;
}

bool read_status(PowerStatus& out) {
    uint8_t s[2] = {};
    if (!rd(kRegStatus1, s, 2)) return false;
    out.pmic_ok = true;
    out.vbus = (s[0] >> 5) & 1;
    out.battery_present = (s[0] >> 3) & 1;
    out.charging = (s[1] >> 5) == 0x01;
    out.percent = -1;
    out.vbat_mv = 0;
    if (out.battery_present) {
        uint8_t pct = 0, v[2] = {};
        if (rd(kRegBatPercent, &pct, 1) && pct <= 100) out.percent = pct;
        if (rd(kRegVbatH, v, 2)) out.vbat_mv = ((v[0] & 0x1F) << 8) | v[1];
    }
    return true;
}

const char* boot_rails() { return rails_text; }
const char* boot_regs() { return regs_text; }

}  // namespace board::axp2101
