#include "ccore/pcm.hpp"

namespace ccore {

bool PcmConverter::configure(uint32_t in_rate, uint8_t channels, uint8_t bits) {
    if (in_rate < 4000 || in_rate > 384000 || channels == 0 || channels > 8) return false;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) return false;
    in_rate_ = in_rate;
    channels_ = channels;
    bits_ = bits;
    frame_bytes_ = static_cast<size_t>(channels) * (bits / 8);
    step_ = (static_cast<uint64_t>(in_rate) << 32) / kOutRate;
    reset();
    return true;
}

void PcmConverter::reset() {
    phase_ = 0;
    have_prev_ = false;
    prev_[0] = prev_[1] = 0;
}

int32_t PcmConverter::sample(const uint8_t* f, int ch) const {
    const uint8_t* p = f + static_cast<size_t>(ch) * (bits_ / 8);
    switch (bits_) {
        case 8: return (static_cast<int32_t>(p[0]) - 128) << 8;  // unsigned 8-bit PCM
        case 16: return static_cast<int16_t>(p[0] | (p[1] << 8));
        case 24: return static_cast<int32_t>((p[0] << 8) | (p[1] << 16) | (static_cast<uint32_t>(p[2]) << 24)) >> 16;
        default: return static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24)) >> 16;
    }
}

size_t PcmConverter::process(const uint8_t* in, size_t in_bytes, int16_t* out, size_t out_frames, size_t& consumed) {
    size_t in_frames = in_bytes / frame_bytes_;
    size_t i = 0, o = 0;
    auto frame_lr = [&](size_t idx, int16_t& l, int16_t& r) {
        const uint8_t* f = in + idx * frame_bytes_;
        int32_t a = sample(f, 0);
        int32_t b = channels_ >= 2 ? sample(f, 1) : a;  // mono -> both ears; extra channels dropped
        l = static_cast<int16_t>(a);
        r = static_cast<int16_t>(b);
    };
    if (!have_prev_ && in_frames > 0) {
        frame_lr(0, prev_[0], prev_[1]);
        have_prev_ = true;
        i = 1;
    }
    while (o < out_frames && i < in_frames) {
        int16_t nl, nr;
        frame_lr(i, nl, nr);
        // Emit output samples between prev_ (phase 0) and the next input frame (phase 1).
        while (phase_ < (1ull << 32) && o < out_frames) {
            int64_t t = static_cast<int64_t>(phase_ >> 16);  // 16-bit fraction
            out[2 * o] = static_cast<int16_t>(prev_[0] + (((nl - prev_[0]) * t) >> 16));
            out[2 * o + 1] = static_cast<int16_t>(prev_[1] + (((nr - prev_[1]) * t) >> 16));
            ++o;
            phase_ += step_;
        }
        if (phase_ >= (1ull << 32)) {
            phase_ -= 1ull << 32;
            prev_[0] = nl;
            prev_[1] = nr;
            ++i;
        }
    }
    consumed = i * frame_bytes_;
    return o;
}

}  // namespace ccore
