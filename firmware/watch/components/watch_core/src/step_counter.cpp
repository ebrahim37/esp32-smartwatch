#include "core/step_counter.hpp"

#include <cmath>

namespace core {

uint32_t StepCounter::add_sample(float ax, float ay, float az, int64_t t_us) {
    float mag = std::sqrt(ax * ax + ay * ay + az * az);
    if (!init_) {
        init_ = true;
        baseline_ = mag;
        last_t_ = t_us;
        return 0;
    }
    float dt = static_cast<float>(t_us - last_t_) / 1e6f;
    last_t_ = t_us;
    if (dt <= 0.f || dt > 0.5f) {
        // Sampling gap: restart filters rather than inventing a step.
        baseline_ = mag;
        smooth_ = 0.f;
        armed_ = true;
        return 0;
    }
    // First-order low-pass coefficients from time constants (robust to jittery sampling).
    float a_base = dt / (1.5f + dt);    // ~1.5 s gravity baseline
    float a_smooth = dt / (0.05f + dt); // ~3 Hz smoothing
    baseline_ += a_base * (mag - baseline_);
    smooth_ += a_smooth * ((mag - baseline_) - smooth_);

    uint32_t credited = 0;
    if (armed_ && smooth_ > cfg_.threshold_g) {
        armed_ = false;
        int64_t since = t_us - last_step_t_;
        if (last_step_t_ != 0 && since < cfg_.min_interval_us) {
            // Bounce within one stride: ignore.
        } else {
            bool regular = last_step_t_ != 0 && since <= cfg_.max_interval_us;
            last_step_t_ = t_us;
            if (walking_ && regular) {
                credited = 1;
            } else if (regular) {
                if (++pending_ >= cfg_.regularity_steps) {
                    walking_ = true;
                    credited = static_cast<uint32_t>(pending_);
                    pending_ = 0;
                }
            } else {
                walking_ = false;
                pending_ = 1;
            }
        }
    } else if (!armed_ && smooth_ < -cfg_.threshold_g * 0.5f) {
        armed_ = true;
    }
    if (walking_ && last_step_t_ != 0 && t_us - last_step_t_ > cfg_.max_interval_us) {
        walking_ = false;
        pending_ = 0;
    }
    total_ += credited;
    return credited;
}

}  // namespace core
