#include "core/heading.hpp"

#include <cmath>

namespace core {

void RelativeHeading::update(float ax, float ay, float az, float gx, float gy, float gz, int64_t t_us) {
    float an = std::sqrt(ax * ax + ay * ay + az * az);
    if (!init_) {
        init_ = true;
        last_t_ = t_us;
        if (an > 0.3f) {
            grav_[0] = ax / an;
            grav_[1] = ay / an;
            grav_[2] = az / an;
        }
        return;
    }
    float dt = static_cast<float>(t_us - last_t_) / 1e6f;
    last_t_ = t_us;
    if (dt <= 0.f || dt > 0.5f) return;

    if (an > 0.3f) {
        float k = dt / (0.3f + dt);
        for (int i = 0; i < 3; ++i) grav_[i] += k * ((i == 0 ? ax : i == 1 ? ay : az) / an - grav_[i]);
        float gn = std::sqrt(grav_[0] * grav_[0] + grav_[1] * grav_[1] + grav_[2] * grav_[2]);
        if (gn > 1e-3f)
            for (float& g : grav_) g /= gn;
    }

    float w[3] = {gx - bias_[0], gy - bias_[1], gz - bias_[2]};
    float wmag = std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
    bool still = wmag < 2.0f && std::fabs(an - 1.0f) < 0.05f;
    if (still) {
        if (still_since_ == 0) still_since_ = t_us;
        if (t_us - still_since_ > 1000000) {
            float k = dt / (5.0f + dt);
            bias_[0] += k * (gx - bias_[0]);
            bias_[1] += k * (gy - bias_[1]);
            bias_[2] += k * (gz - bias_[2]);
        }
    } else {
        still_since_ = 0;
    }

    // Counter-clockwise rotation about "up" (right-hand rule) decreases a compass-style
    // heading, hence the minus sign.
    float yaw_rate = w[0] * grav_[0] + w[1] * grav_[1] + w[2] * grav_[2];
    if (std::fabs(yaw_rate) < 0.3f) yaw_rate = 0.f;  // deadband against residual bias drift
    heading_ -= yaw_rate * dt;
}

float RelativeHeading::degrees() const {
    float h = std::fmod(heading_, 360.f);
    if (h < 0) h += 360.f;
    return h;
}

}  // namespace core
