// Relative heading from gyroscope integration.
//
// TODO(PROTOTYPE): the prototype's QMI8658 has no magnetometer, so heading is only a
// *relative* estimate: it starts at 0 deg ("north") when reset and integrates yaw rate,
// drifting over time. It must never be presented as a magnetic compass bearing. The custom
// watch's 9-axis IMU will replace this with a tilt-compensated, calibrated magnetometer
// heading fused with the gyro.
//
// Yaw rate is the gyro vector projected onto the gravity direction (from a low-passed
// accelerometer), so the estimate works however the wrist is tilted. A slowly adapting gyro
// bias is learned while the device is still.
#pragma once

#include <cstdint>

namespace core {

class RelativeHeading {
public:
    void reset(float heading_deg = 0.f) { heading_ = heading_deg; }
    void update(float ax, float ay, float az, float gx, float gy, float gz, int64_t t_us);
    float degrees() const;  // 0..360
    bool is_magnetic() const { return false; }

private:
    bool init_ = false;
    float grav_[3] = {0, 0, 1};
    float bias_[3] = {0, 0, 0};
    float heading_ = 0.f;
    int64_t last_t_ = 0;
    int64_t still_since_ = 0;
};

}  // namespace core
