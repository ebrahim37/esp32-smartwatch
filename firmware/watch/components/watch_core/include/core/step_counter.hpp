// Orientation-independent pedometer for a wrist-worn 3-axis accelerometer.
//
// Pipeline (per sample, ~50 Hz):
//   |a| (g) -> gravity removal (slow low-pass baseline) -> smoothing low-pass (~3 Hz)
//   -> hysteresis peak detector (rise above +T, fall below -T/2)
//   -> step interval gate (0.25 .. 2.0 s) -> regularity gate (N consecutive valid steps
//   before any are credited, rejecting isolated arm movements).
#pragma once

#include <cstdint>

namespace core {

struct StepConfig {
    float threshold_g = 0.15f;  // dynamic acceleration needed for a step peak
    int64_t min_interval_us = 250000;
    int64_t max_interval_us = 2000000;
    int regularity_steps = 4;
};

class StepCounter {
public:
    explicit StepCounter(StepConfig cfg = {}) : cfg_(cfg) {}
    void set_config(const StepConfig& cfg) { cfg_ = cfg; }
    const StepConfig& config() const { return cfg_; }

    // Returns the number of newly credited steps (usually 0 or 1; N when the regularity
    // gate opens).
    uint32_t add_sample(float ax, float ay, float az, int64_t t_us);

    uint32_t total() const { return total_; }
    void set_total(uint32_t t) { total_ = t; }

private:
    StepConfig cfg_;
    bool init_ = false;
    float baseline_ = 1.0f;
    float smooth_ = 0.0f;
    int64_t last_t_ = 0;
    bool armed_ = true;      // waiting for a rising edge
    int64_t last_step_t_ = 0;
    int pending_ = 0;        // steps seen while the regularity gate is closed
    bool walking_ = false;
    uint32_t total_ = 0;
};

}  // namespace core
