#include "app/tiles.hpp"

#include <algorithm>
#include <deque>
#include <mutex>

#include "app/state.hpp"
#include "ble/link.hpp"
#include "core/swt.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "miniz.h"  // ESP32-S3 ROM inflate (tinfl)
#include "swp/wire.hpp"

namespace tiles {
namespace {

constexpr size_t kMaxCached = 96;      // compressed SWT, a few KB each in PSRAM
constexpr size_t kMaxInFlight = 2;
constexpr int64_t kRequestTimeoutUs = 20 * 1000000;

struct Entry {
    Key k;
    Blob swt;
};
struct Pending {
    Key k;
    int64_t sent_us;
};

std::mutex mu;
std::deque<Entry> cache;  // front = most recent
std::vector<Key> wanted;
std::vector<Pending> in_flight;
std::vector<Key> failed;

bool contains(const std::vector<Key>& v, const Key& k) { return std::find(v.begin(), v.end(), k) != v.end(); }

}  // namespace

void init() {}

Blob get(const Key& k) {
    std::lock_guard<std::mutex> g(mu);
    for (size_t i = 0; i < cache.size(); ++i)
        if (cache[i].k == k) {
            Entry e = cache[i];
            cache.erase(cache.begin() + static_cast<long>(i));
            cache.push_front(e);
            return e.swt;
        }
    return nullptr;
}

void want(const std::vector<Key>& keys) {
    {
        std::lock_guard<std::mutex> g(mu);
        wanted.clear();
        for (auto& k : keys) {
            bool cached = std::any_of(cache.begin(), cache.end(), [&](const Entry& e) { return e.k == k; });
            if (!cached && !contains(failed, k)) wanted.push_back(k);
        }
    }
    pump();
}

bool decode(const Blob& swt, uint16_t* out) {
    static tinfl_decompressor* inflator = nullptr;
    static uint8_t* indices = nullptr;
    if (!inflator) inflator = static_cast<tinfl_decompressor*>(heap_caps_malloc(sizeof(tinfl_decompressor), MALLOC_CAP_SPIRAM));
    if (!indices) indices = static_cast<uint8_t*>(heap_caps_malloc(core::kSwtIndexBytes, MALLOC_CAP_SPIRAM));
    core::SwtHeader h;
    if (!swt || !inflator || !indices || !core::parse_swt(swt->data(), swt->size(), h)) return false;
    tinfl_init(inflator);
    size_t in_len = swt->size() - h.payload_offset, out_len = core::kSwtIndexBytes;
    tinfl_status st = tinfl_decompress(inflator, swt->data() + h.payload_offset, &in_len, indices, indices, &out_len,
                                       TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    if (st != TINFL_STATUS_DONE || out_len != core::kSwtIndexBytes) {
        ESP_LOGW("tiles", "bad SWT tile (inflate status %d, %u bytes)", static_cast<int>(st), static_cast<unsigned>(out_len));
        return false;
    }
    core::expand_i4(indices, h.palette565, out, core::kSwtSize * core::kSwtSize);
    return true;
}

void on_tile(const Key& k, std::vector<uint8_t>&& swt) {
    {
        std::lock_guard<std::mutex> g(mu);
        in_flight.erase(std::remove_if(in_flight.begin(), in_flight.end(), [&](const Pending& p) { return p.k == k; }), in_flight.end());
        wanted.erase(std::remove(wanted.begin(), wanted.end(), k), wanted.end());
        cache.push_front({k, std::make_shared<const std::vector<uint8_t>>(std::move(swt))});
        while (cache.size() > kMaxCached) cache.pop_back();
    }
    app::mark(app::kDirtyTiles);
    pump();
}

void on_failed(const Key& k) {
    {
        std::lock_guard<std::mutex> g(mu);
        in_flight.erase(std::remove_if(in_flight.begin(), in_flight.end(), [&](const Pending& p) { return p.k == k; }), in_flight.end());
        wanted.erase(std::remove(wanted.begin(), wanted.end(), k), wanted.end());
        if (failed.size() > 32) failed.erase(failed.begin());
        failed.push_back(k);  // don't hammer the companion; cleared on reconnect
    }
    app::mark(app::kDirtyTiles);
    pump();
}

void on_link_reset() {
    std::lock_guard<std::mutex> g(mu);
    in_flight.clear();
    failed.clear();
}

void pump() {
    std::vector<Key> to_send;
    {
        std::lock_guard<std::mutex> g(mu);
        int64_t now = esp_timer_get_time();
        in_flight.erase(std::remove_if(in_flight.begin(), in_flight.end(), [&](const Pending& p) { return now - p.sent_us > kRequestTimeoutUs; }),
                        in_flight.end());
        for (auto& k : wanted) {
            if (in_flight.size() + to_send.size() >= kMaxInFlight) break;
            bool busy = std::any_of(in_flight.begin(), in_flight.end(), [&](const Pending& p) { return p.k == k; });
            if (!busy) to_send.push_back(k);
        }
        for (auto& k : to_send) in_flight.push_back({k, now});
    }
    for (auto& k : to_send) {
        swp::MapTileRequest r;
        r.z = k.z;
        r.x = k.x;
        r.y = k.y;
        swlink::send(r);
    }
}

std::vector<uint8_t> encode_meta(const Key& k) {
    std::vector<uint8_t> m;
    swp::Writer w(m);
    w.u8(k.z);
    w.u32(k.x);
    w.u32(k.y);
    return m;
}

bool decode_meta(const std::vector<uint8_t>& meta, Key& k) {
    swp::Reader r(meta.data(), meta.size());
    k.z = r.u8();
    k.x = r.u32();
    k.y = r.u32();
    return !r.failed();
}

}  // namespace tiles
