// Minimal AXP2101 PMIC access: status, fuel gauge and battery voltage.
#pragma once

#include "board.hpp"
#include "driver/i2c_master.h"

namespace board::axp2101 {

esp_err_t init(i2c_master_bus_handle_t bus);
bool read_status(PowerStatus& out);

}  // namespace board::axp2101
