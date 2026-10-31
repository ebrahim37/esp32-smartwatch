#include "ccore/quota.hpp"

#include <algorithm>

namespace ccore {

bool can_store(const Quota& q, uint64_t bytes, uint64_t card_free) {
    return q.fits(bytes) && card_free >= bytes + kMinFreeReserve;
}

void LruIndex::put(const std::string& key, uint64_t size, uint64_t now) {
    for (auto& e : entries_)
        if (e.key == key) {
            used_ = used_ - e.size + size;
            e.size = size;
            e.last_access = now;
            return;
        }
    entries_.push_back({key, size, now});
    used_ += size;
}

void LruIndex::add(const std::string& key, int64_t delta, uint64_t now) {
    for (auto& e : entries_)
        if (e.key == key) {
            uint64_t ns = delta < 0 && static_cast<uint64_t>(-delta) > e.size ? 0 : e.size + delta;
            used_ = used_ - e.size + ns;
            e.size = ns;
            if (now > e.last_access) e.last_access = now;
            return;
        }
    if (delta > 0) put(key, static_cast<uint64_t>(delta), now);
}

bool LruIndex::touch(const std::string& key, uint64_t now) {
    for (auto& e : entries_)
        if (e.key == key) {
            e.last_access = now;
            return true;
        }
    return false;
}

bool LruIndex::remove(const std::string& key) {
    for (size_t i = 0; i < entries_.size(); ++i)
        if (entries_[i].key == key) {
            used_ -= entries_[i].size;
            entries_[i] = entries_.back();
            entries_.pop_back();
            return true;
        }
    return false;
}

bool LruIndex::contains(const std::string& key) const {
    return std::any_of(entries_.begin(), entries_.end(), [&](const CacheEntry& e) { return e.key == key; });
}

std::vector<std::string> LruIndex::plan_eviction(uint64_t incoming, int low_watermark_pct) const {
    std::vector<std::string> out;
    if (used_ + incoming <= limit_) return out;
    uint64_t target = limit_ / 100 * static_cast<uint64_t>(low_watermark_pct);
    if (incoming > target) target = 0; else target -= incoming;
    uint64_t used = used_;
    for (auto* e : oldest_first()) {
        if (used <= target) break;
        out.push_back(e->key);
        used -= e->size;
    }
    return out;
}

std::vector<const CacheEntry*> LruIndex::oldest_first() const {
    std::vector<const CacheEntry*> order;
    order.reserve(entries_.size());
    for (auto& e : entries_) order.push_back(&e);
    std::sort(order.begin(), order.end(), [](const CacheEntry* a, const CacheEntry* b) { return a->last_access < b->last_access; });
    return order;
}

std::vector<std::string> LruIndex::plan_free_bytes(uint64_t bytes) const {
    std::vector<std::string> out;
    uint64_t freed = 0;
    for (auto* e : oldest_first()) {
        if (freed >= bytes) break;
        out.push_back(e->key);
        freed += e->size;
    }
    return out;
}

}  // namespace ccore
