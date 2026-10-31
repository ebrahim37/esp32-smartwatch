// Routes decoded SWP frames from BLE sessions to companion subsystems. Runs in the Bluedroid
// callback task: anything slow (HTTP, SD-heavy work) is handed to worker tasks.
#pragma once

#include <cstdint>

#include "swp/frame.hpp"

namespace dispatch {

void init();
void on_frame(uint16_t conn, const swp::Frame& f);
void on_disconnect(uint16_t conn, uint8_t role);
void broadcast_status();  // CompanionStatus + WifiStatus to the watch

}  // namespace dispatch
