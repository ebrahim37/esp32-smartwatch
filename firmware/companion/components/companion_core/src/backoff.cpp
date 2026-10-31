#include "ccore/backoff.hpp"

namespace ccore {

uint32_t Backoff::next(uint32_t rnd) {
    uint64_t d = init_;
    for (uint32_t i = 0; i < attempt_ && d < max_; ++i) d *= 2;
    if (d > max_) d = max_;
    if (attempt_ < 31) ++attempt_;
    uint64_t span = d * jitter_ / 100;
    if (span > 0) d = d - span + (rnd % (2 * span + 1));
    if (d > max_) d = max_;
    return static_cast<uint32_t>(d);
}

}  // namespace ccore
