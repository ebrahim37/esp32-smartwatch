// microSD storage: SDMMC 1-bit bring-up, non-destructive FAT32 detection, conditional
// provisioning (format), directory layout, quota accounting and the map tile LRU cache.
//
// Freenove FNK0060 (verified against the vendor SDMMC sketch and docs): 1-bit SDMMC on
// slot 1 IOMUX pins CLK=GPIO14, CMD=GPIO15, D0=GPIO2. No card-detect pin.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "swp/messages.hpp"

namespace storage {

constexpr const char* kMount = "/sdcard";
constexpr const char* kMusicDir = "/sdcard/music";
constexpr const char* kMusicTmpDir = "/sdcard/music/.tmp";
constexpr const char* kMapsDir = "/sdcard/maps";

constexpr const char* kMemosDir = "/sdcard/memos";  // TODO(PROTOTYPE): voice memos deferred

struct Info {
    swp::SdState state = swp::SdState::Absent;
    std::string detail;
    uint64_t total = 0;
    uint64_t free = 0;
    uint64_t music_used = 0;
    uint64_t maps_used = 0;
};

// Starts the storage task (probe, provisioning, mount; re-probes while no card is usable).
void start();
Info info();
bool mounted();

// ---- Music quota (2 GB) -----------------------------------------------------------
bool music_reserve(uint64_t bytes);  // admission check before a download
void music_add(int64_t delta);       // account completed/removed files
void music_set_used(uint64_t bytes);

// ---- Map tile cache (4 GB, LRU) ---------------------------------------------------
std::string tile_path(uint8_t z, uint32_t x, uint32_t y);
// Reads a cached tile and marks it recently used. Returns false if absent.
bool tile_read(uint8_t z, uint32_t x, uint32_t y, std::vector<uint8_t>& out);
bool tile_exists(uint8_t z, uint32_t x, uint32_t y);
// Tiles live under maps/<style>, the helper's `tile_style` (e.g. "2x"). Until it is known the
// cache is unused; switching style deletes the other trees in the background.
void set_tile_style(const std::string& style);
std::string tiles_dir();  // empty until the style is known
// Stores a tile atomically (temp file + rename), evicting least-recently-used blocks.
bool tile_write(uint8_t z, uint32_t x, uint32_t y, const std::vector<uint8_t>& data);

// Atomic file helpers.
bool write_file_atomic(const std::string& path, const uint8_t* data, size_t len, bool sync = true);
bool read_file(const std::string& path, std::vector<uint8_t>& out, size_t max_len);
uint64_t file_size(const std::string& path);  // 0 if missing
void flush_index();  // persist the LRU index if dirty (called periodically)

}  // namespace storage
