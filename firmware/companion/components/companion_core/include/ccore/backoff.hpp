// Exponential retry backoff with jitter, used for Wi-Fi reconnects, HTTP retries and BLE
// reconnection scheduling.
#pragma once

#include <cstdint>

namespace ccore {

class Backoff {
public:
    Backoff(uint32_t initial_ms, uint32_t max_ms, uint32_t jitter_pct = 20) : init_(initial_ms), max_(max_ms), jitter_(jitter_pct) {}
    // Delay before the next attempt. `rnd` is any random 32-bit value (esp_random on device).
    uint32_t next(uint32_t rnd);
    void reset() { attempt_ = 0; }
    uint32_t attempts() const { return attempt_; }

private:
    uint32_t init_, max_, jitter_;
    uint32_t attempt_ = 0;
};

}  // namespace ccore
