// Per-category SD storage quotas and least-recently-used eviction for the map cache.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ccore {

constexpr uint64_t kMiB = 1024ull * 1024;
constexpr uint64_t kGiB = 1024ull * kMiB;
constexpr uint64_t kMusicQuota = 2 * kGiB;
constexpr uint64_t kMapsQuota = 4 * kGiB;
constexpr uint64_t kMemosQuota = 256 * kMiB;  // TODO(PROTOTYPE): voice memos are deferred
constexpr uint64_t kMinFreeReserve = 256 * kMiB;  // never fill the card completely

struct Quota {
    uint64_t limit = 0;
    uint64_t used = 0;
    bool fits(uint64_t extra) const { return used + extra <= limit; }
    uint64_t available() const { return used >= limit ? 0 : limit - used; }
};

// Admission check combining the category quota with the card-wide free-space reserve.
bool can_store(const Quota& q, uint64_t bytes, uint64_t card_free);

struct CacheEntry {
    std::string key;
    uint64_t size = 0;
    uint64_t last_access = 0;  // monotonic counter or UTC seconds
};

// LRU accounting for the map tile cache. Keys are cache-relative paths.
class LruIndex {
public:
    explicit LruIndex(uint64_t limit) : limit_(limit) {}

    void put(const std::string& key, uint64_t size, uint64_t now);
    // Adds `delta` bytes to an entry (creating it), e.g. one more tile in a cache block.
    void add(const std::string& key, int64_t delta, uint64_t now);
    bool touch(const std::string& key, uint64_t now);  // returns false if missing
    bool remove(const std::string& key);
    bool contains(const std::string& key) const;

    // Entries to delete so that `incoming` more bytes fit while staying under
    // low_watermark_pct of the limit (hysteresis avoids evicting on every insert).
    std::vector<std::string> plan_eviction(uint64_t incoming, int low_watermark_pct = 90) const;
    // Oldest entries whose removal frees at least `bytes` (card-wide free-space pressure).
    std::vector<std::string> plan_free_bytes(uint64_t bytes) const;
    const std::vector<CacheEntry>& entries() const { return entries_; }

    uint64_t used() const { return used_; }
    uint64_t limit() const { return limit_; }
    size_t count() const { return entries_.size(); }

private:
    std::vector<const CacheEntry*> oldest_first() const;
    std::vector<CacheEntry> entries_;
    uint64_t used_ = 0;
    uint64_t limit_;
};

}  // namespace ccore
