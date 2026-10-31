// Playback order: sequential or shuffled, with repeat off / all / one.
#pragma once

#include <cstdint>
#include <vector>

namespace ccore {

enum class Repeat : uint8_t { Off = 0, All = 1, One = 2 };

class PlayQueue {
public:
    void load(uint16_t count, uint16_t start_index, bool shuffle, uint32_t seed);
    bool empty() const { return order_.empty(); }
    int current() const { return order_.empty() ? -1 : order_[pos_]; }

    // Natural end of track (`user` = false) or Next button (`user` = true).
    // Returns false when playback should stop.
    bool next(bool user);
    // Previous button: restarts the current track if it has played > 3 s (caller decides
    // via `restart_threshold_passed`), otherwise moves back.
    bool prev(bool restart_threshold_passed);

    void set_shuffle(bool on, uint32_t seed);  // keeps the current track playing
    void set_repeat(Repeat r) { repeat_ = r; }
    bool shuffle() const { return shuffle_; }
    Repeat repeat() const { return repeat_; }
    uint16_t size() const { return static_cast<uint16_t>(order_.size()); }

private:
    void shuffle_from(size_t start, uint32_t seed);
    std::vector<uint16_t> order_;
    size_t pos_ = 0;
    bool shuffle_ = false;
    Repeat repeat_ = Repeat::Off;
    bool restart_ = false;
};

}  // namespace ccore
