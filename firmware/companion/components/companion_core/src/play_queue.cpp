#include "ccore/play_queue.hpp"

#include <algorithm>

namespace ccore {

namespace {
uint32_t xorshift(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}
}  // namespace

void PlayQueue::shuffle_from(size_t start, uint32_t seed) {
    uint32_t s = seed ? seed : 0x9E3779B9u;
    for (size_t i = order_.size(); i > start + 1; --i) {
        size_t j = start + xorshift(s) % (i - start);
        std::swap(order_[i - 1], order_[j]);
    }
}

void PlayQueue::load(uint16_t count, uint16_t start_index, bool shuffle, uint32_t seed) {
    order_.resize(count);
    for (uint16_t i = 0; i < count; ++i) order_[i] = i;
    pos_ = 0;
    shuffle_ = shuffle;
    if (count == 0) return;
    if (start_index >= count) start_index = 0;
    if (shuffle) {
        std::swap(order_[0], order_[start_index]);
        shuffle_from(1, seed);
    } else {
        pos_ = start_index;
    }
}

bool PlayQueue::next(bool user) {
    if (order_.empty()) return false;
    if (!user && repeat_ == Repeat::One) return true;
    if (pos_ + 1 < order_.size()) {
        ++pos_;
        return true;
    }
    if (repeat_ == Repeat::All || (user && repeat_ == Repeat::One)) {
        pos_ = 0;
        return true;
    }
    return false;
}

bool PlayQueue::prev(bool restart_threshold_passed) {
    if (order_.empty()) return false;
    if (restart_threshold_passed) return true;
    if (pos_ > 0) {
        --pos_;
    } else if (repeat_ == Repeat::All) {
        pos_ = order_.size() - 1;
    }
    return true;
}

void PlayQueue::set_shuffle(bool on, uint32_t seed) {
    if (on == shuffle_ || order_.empty()) {
        shuffle_ = on;
        return;
    }
    uint16_t cur = order_[pos_];
    for (uint16_t i = 0; i < order_.size(); ++i) order_[i] = i;
    shuffle_ = on;
    if (on) {
        std::swap(order_[0], order_[cur]);
        pos_ = 0;
        shuffle_from(1, seed);
    } else {
        pos_ = cur;
    }
}

}  // namespace ccore
