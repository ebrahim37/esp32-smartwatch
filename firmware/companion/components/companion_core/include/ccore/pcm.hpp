// PCM conversion for the A2DP path: any decoder output (8/16/24/32-bit, mono/stereo/multi,
// any sample rate) -> 44.1 kHz 16-bit stereo interleaved. Linear interpolation is used for
// rate conversion: cheap and allocation-free (the hot audio path never allocates).
#pragma once

#include <cstddef>
#include <cstdint>

namespace ccore {

class PcmConverter {
public:
    static constexpr uint32_t kOutRate = 44100;

    bool configure(uint32_t in_rate, uint8_t channels, uint8_t bits);
    void reset();

    // Converts as much of `in` as fits into `out` (stereo frames). Returns frames written
    // and sets `consumed` (bytes of input used; a partial trailing input frame is left).
    size_t process(const uint8_t* in, size_t in_bytes, int16_t* out, size_t out_frames, size_t& consumed);

    uint32_t in_rate() const { return in_rate_; }

private:
    int32_t sample(const uint8_t* frame, int ch) const;

    uint32_t in_rate_ = kOutRate;
    uint8_t channels_ = 2;
    uint8_t bits_ = 16;
    size_t frame_bytes_ = 4;
    uint64_t step_ = 1ull << 32;  // input frames per output frame, 32.32 fixed point
    uint64_t phase_ = 0;          // fractional position between prev_ and the next frame
    int16_t prev_[2] = {0, 0};
    bool have_prev_ = false;
};

}  // namespace ccore
