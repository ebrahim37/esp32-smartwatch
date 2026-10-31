// Map tile cache on the watch (compressed SWT tiles in PSRAM, LRU) and request scheduling.
// Tiles arrive from the companion through chunked SWP transfers.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace tiles {

struct Key {
    uint8_t z;
    uint32_t x, y;
    bool operator==(const Key& o) const { return z == o.z && x == o.x && y == o.y; }
};

// Shared, immutable SWT bytes (a few KB each).
using Blob = std::shared_ptr<const std::vector<uint8_t>>;

void init();
Blob get(const Key& k);  // nullptr if not cached
// Queues a request unless cached or in flight. Requests beyond the current wanted set are
// dropped when `set_wanted` is called with a new view.
void want(const std::vector<Key>& keys);
void on_tile(const Key& k, std::vector<uint8_t>&& swt);
// Inflates an SWT tile into 256x256 RGB565 pixels (LVGL task only; shared scratch buffers).
bool decode(const Blob& swt, uint16_t* out);
void on_failed(const Key& k);
void on_link_reset();
void pump();  // sends the next requests (called from link task context)
std::vector<uint8_t> encode_meta(const Key& k);
bool decode_meta(const std::vector<uint8_t>& meta, Key& k);

}  // namespace tiles
