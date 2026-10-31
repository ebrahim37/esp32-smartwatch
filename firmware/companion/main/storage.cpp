#include "storage.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

#include "ccore/quota.hpp"
#include "ccore/sd_policy.hpp"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "driver/sdmmc_host.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include "swp/wire.hpp"

namespace storage {
namespace {

constexpr const char* TAG = "storage";
constexpr const char* kIndexPath = "/sdcard/maps/lru.idx";
constexpr int kTileBlockShift = 3;  // LRU granularity: 8x8 tile blocks per zoom level

std::mutex mu;
Info inf;
sdmmc_card_t* card = nullptr;
FATFS* fs = nullptr;
BYTE pdrv = 0xFF;
bool host_up = false;
ccore::Quota music_q{ccore::kMusicQuota, 0};
std::unique_ptr<ccore::LruIndex> lru;
bool lru_dirty = false;
int64_t lru_saved_us = 0;
uint64_t access_clock = 0;

void set_state(swp::SdState st, const std::string& detail) {
    std::lock_guard<std::mutex> g(mu);
    inf.state = st;
    inf.detail = detail;
}

bool init_card() {
    if (!host_up) {
        if (sdmmc_host_init() != ESP_OK) return false;
        host_up = true;
        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.width = 1;
        slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // board has external pull-ups too
        if (sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot) != ESP_OK) return false;
    }
    if (!card) card = static_cast<sdmmc_card_t*>(heap_caps_calloc(1, sizeof(sdmmc_card_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_1;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;  // 20 MHz: robust on 1-bit with long traces
    host.flags = SDMMC_HOST_FLAG_1BIT;
    return sdmmc_card_init(&host, card) == ESP_OK;
}

bool read_sector(uint32_t lba, uint8_t* buf) {
    // Two attempts per sector; a persistent failure marks the probe unreadable.
    for (int i = 0; i < 2; ++i)
        if (sdmmc_read_sectors(card, buf, lba, 1) == ESP_OK) return true;
    return false;
}

ccore::SdProbe probe() {
    ccore::SdProbe p;
    p.card_present = true;
    auto* buf = static_cast<uint8_t*>(heap_caps_malloc(512, MALLOC_CAP_DMA));
    if (!buf) return p;
    p.read_ok = read_sector(0, buf);
    if (p.read_ok) {
        uint32_t lba = 0;
        uint8_t ptype = 0;
        p.kind = ccore::classify_sector0(buf, lba, ptype);
        if (p.kind == ccore::VolumeKind::Blank) {
            // Sector 0 blank: only call the card blank if typical volume locations are blank
            // too (a damaged MBR in front of a real filesystem must not be formatted).
            const uint32_t probes[] = {1, 63, 2048, 8192, 32768};
            for (uint32_t s : probes) {
                if (s >= card->csd.capacity) continue;
                if (!read_sector(s, buf)) {
                    p.read_ok = false;
                    break;
                }
                if (!ccore::sector_blank(buf)) {
                    ccore::VolumeKind k = ccore::classify_vbr(buf);
                    p.kind = k == ccore::VolumeKind::Blank ? ccore::VolumeKind::Unknown : k;
                    break;
                }
            }
        } else if (lba != 0 && lba < card->csd.capacity) {
            // Confirm the partition's own boot record.
            if (!read_sector(lba, buf)) {
                p.read_ok = false;
            } else {
                ccore::VolumeKind vbr = ccore::classify_vbr(buf);
                if (vbr == ccore::VolumeKind::Fat32 || vbr == ccore::VolumeKind::FatOther || vbr == ccore::VolumeKind::ExFat ||
                    vbr == ccore::VolumeKind::Ntfs)
                    p.kind = vbr;
                else if (vbr == ccore::VolumeKind::Blank)
                    p.kind = ccore::VolumeKind::Blank;  // partitioned but never formatted
                else if (p.kind == ccore::VolumeKind::Fat32 || p.kind == ccore::VolumeKind::FatOther)
                    p.kind = ccore::VolumeKind::Unknown;  // FAT type id but no valid FAT VBR
            }
        }
    }
    heap_caps_free(buf);
    return p;
}

bool format_fat32() {
    ESP_LOGW(TAG, "formatting card as FAT32");
    set_state(swp::SdState::Formatting, "formatting FAT32");
    if (ff_diskio_get_drive(&pdrv) != ESP_OK) return false;
    ff_diskio_register_sdmmc(pdrv, card);
    const size_t work_sz = 4096;
    void* work = heap_caps_malloc(work_sz, MALLOC_CAP_DEFAULT);
    char drv[3] = {static_cast<char>('0' + pdrv), ':', 0};
    LBA_t plist[] = {100, 0, 0, 0};
    FRESULT fr = work ? f_fdisk(pdrv, plist, work) : FR_NOT_ENOUGH_CORE;
    if (fr == FR_OK) {
        MKFS_PARM opt = {};
        opt.fmt = FM_FAT32;
        opt.n_fat = 2;
        opt.au_size = 32 * 1024;
        fr = f_mkfs(drv, &opt, work, work_sz);
    }
    free(work);
    ff_diskio_unregister(pdrv);
    pdrv = 0xFF;
    if (fr != FR_OK) ESP_LOGE(TAG, "format failed: FRESULT %d", fr);
    return fr == FR_OK;
}

bool mount() {
    if (ff_diskio_get_drive(&pdrv) != ESP_OK) return false;
    ff_diskio_register_sdmmc(pdrv, card);
    char drv[3] = {static_cast<char>('0' + pdrv), ':', 0};
    esp_vfs_fat_conf_t conf = {kMount, drv, 8};
    if (esp_vfs_fat_register_cfg(&conf, &fs) != ESP_OK) {
        ff_diskio_unregister(pdrv);
        return false;
    }
    FRESULT fr = f_mount(fs, drv, 1);
    if (fr != FR_OK) {
        ESP_LOGE(TAG, "f_mount failed: %d", fr);
        esp_vfs_fat_unregister_path(kMount);
        ff_diskio_unregister(pdrv);
        fs = nullptr;
        return false;
    }
    return true;
}

void ensure_dir(const char* p) {
    struct stat st;
    if (stat(p, &st) != 0) mkdir(p, 0775);
}

uint64_t dir_size(const std::string& path, int depth = 0) {
    uint64_t total = 0;
    DIR* d = opendir(path.c_str());
    if (!d) return 0;
    while (struct dirent* e = readdir(d)) {
        if (e->d_name[0] == '.' && (e->d_name[1] == 0 || e->d_name[1] == '.')) continue;
        std::string p = path + "/" + e->d_name;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (depth < 4) total += dir_size(p, depth + 1);
        } else {
            total += st.st_size;
        }
        if ((total & 0xFFFFF) == 0) vTaskDelay(1);  // yield on large scans
    }
    closedir(d);
    return total;
}

std::string block_key(uint8_t z, uint32_t x, uint32_t y) {
    char b[32];
    snprintf(b, sizeof b, "%u/%lu/%lu", z, (unsigned long)(x >> kTileBlockShift), (unsigned long)(y >> kTileBlockShift));
    return b;
}

void load_index() {
    lru = std::make_unique<ccore::LruIndex>(ccore::kMapsQuota);
    std::vector<uint8_t> blob;
    if (read_file(kIndexPath, blob, 4 * 1024 * 1024)) {
        swp::Reader r(blob.data(), blob.size());
        if (r.u8() == 1) {
            access_clock = r.u64();
            uint32_t n = r.u32();
            for (uint32_t i = 0; i < n && !r.failed(); ++i) {
                std::string k = r.str8();
                uint64_t sz = r.u64();
                uint64_t t = r.u64();
                if (!r.failed()) lru->put(k, sz, t);
            }
            if (!r.failed()) return;
        }
        lru = std::make_unique<ccore::LruIndex>(ccore::kMapsQuota);
    }
    // No (valid) index: rebuild from the directory tree. Access order is unknown, so every
    // block starts equally old.
    ESP_LOGI(TAG, "rebuilding map cache index");
    DIR* zd = opendir(kMapsDir);
    if (!zd) return;
    while (struct dirent* ze = readdir(zd)) {
        int z = atoi(ze->d_name);
        if (z <= 0 || z > 20) continue;
        std::string zp = std::string(kMapsDir) + "/" + ze->d_name;
        DIR* xd = opendir(zp.c_str());
        if (!xd) continue;
        while (struct dirent* xe = readdir(xd)) {
            if (xe->d_name[0] == '.') continue;
            uint32_t x = strtoul(xe->d_name, nullptr, 10);
            std::string xp = zp + "/" + xe->d_name;
            DIR* yd = opendir(xp.c_str());
            if (!yd) continue;
            while (struct dirent* ye = readdir(yd)) {
                if (ye->d_name[0] == '.') continue;
                uint32_t y = strtoul(ye->d_name, nullptr, 10);
                struct stat st;
                std::string yp = xp + "/" + ye->d_name;
                if (stat(yp.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                    lru->add(block_key(static_cast<uint8_t>(z), x, y), st.st_size, 0);
                }
            }
            closedir(yd);
            vTaskDelay(1);
        }
        closedir(xd);
    }
    closedir(zd);
    lru_dirty = true;
}

void evict_block(const std::string& key) {
    unsigned z = 0;
    unsigned long bx = 0, by = 0;
    if (sscanf(key.c_str(), "%u/%lu/%lu", &z, &bx, &by) != 3) return;
    for (uint32_t dx = 0; dx < (1u << kTileBlockShift); ++dx)
        for (uint32_t dy = 0; dy < (1u << kTileBlockShift); ++dy) {
            uint32_t x = (bx << kTileBlockShift) + dx, y = (by << kTileBlockShift) + dy;
            unlink(tile_path(static_cast<uint8_t>(z), x, y).c_str());
        }
    lru->remove(key);
    lru_dirty = true;
}

void refresh_usage() {
    uint64_t total = 0, freeb = 0;
    esp_vfs_fat_info(kMount, &total, &freeb);
    std::lock_guard<std::mutex> g(mu);
    inf.total = total;
    inf.free = freeb;
    inf.music_used = music_q.used;
    inf.maps_used = lru ? lru->used() : 0;
}

bool bring_up() {
    if (!init_card()) {
        set_state(swp::SdState::Absent, "no card or card not responding");
        return false;
    }
    ESP_LOGI(TAG, "card: %s, %llu MB", card->cid.name, (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024));
    ccore::SdProbe p = probe();
    ccore::SdDecision d = ccore::decide(p);
    ESP_LOGI(TAG, "probe: read_ok=%d kind=%s -> %s", p.read_ok, ccore::volume_kind_name(p.kind), d.reason.c_str());
    if (d.action == ccore::SdAction::Refuse) {
        set_state(p.read_ok ? swp::SdState::Unsupported : swp::SdState::Error, d.reason);
        return false;
    }
    if (d.action == ccore::SdAction::Format) {
        if (d.destroys_data)
            ESP_LOGW(TAG, "card holds an unsupported filesystem (%s): FORMATTING ERASES ALL EXISTING FILES", ccore::volume_kind_name(p.kind));
        if (!format_fat32()) {
            set_state(swp::SdState::Error, "format failed");
            return false;
        }
    }
    if (!mount()) {
        ccore::SdDecision f = ccore::on_mount_failure();
        set_state(swp::SdState::Error, f.reason);
        return false;
    }
    ensure_dir(kMusicDir);
    ensure_dir(kMusicTmpDir);
    ensure_dir(kMapsDir);
    ensure_dir(kMemosDir);
    set_state(swp::SdState::Mounted, d.action == ccore::SdAction::Format ? "formatted FAT32" : ccore::volume_kind_name(p.kind));
    load_index();
    music_q.used = dir_size(kMusicDir);
    refresh_usage();
    auto i = info();
    ESP_LOGI(TAG, "mounted: total %llu MB, free %llu MB, music %llu MB, maps %llu MB", i.total >> 20, i.free >> 20, i.music_used >> 20,
             i.maps_used >> 20);
    return true;
}

void storage_task(void*) {
    // Retry while no usable card (hot-plug); never retries formatting decisions on I/O errors.
    while (!bring_up()) vTaskDelay(pdMS_TO_TICKS(30000));
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(60000));
        refresh_usage();
        flush_index();
    }
}

}  // namespace

void start() { xTaskCreatePinnedToCore(storage_task, "storage", 6144, nullptr, 3, nullptr, 0); }

Info info() {
    std::lock_guard<std::mutex> g(mu);
    return inf;
}

bool mounted() { return info().state == swp::SdState::Mounted; }

bool music_reserve(uint64_t bytes) {
    std::lock_guard<std::mutex> g(mu);
    return ccore::can_store(music_q, bytes, inf.free);
}

void music_add(int64_t delta) {
    std::lock_guard<std::mutex> g(mu);
    if (delta < 0 && static_cast<uint64_t>(-delta) > music_q.used) music_q.used = 0;
    else music_q.used += delta;
    inf.music_used = music_q.used;
}

void music_set_used(uint64_t b) {
    std::lock_guard<std::mutex> g(mu);
    music_q.used = b;
    inf.music_used = b;
}

std::string tile_path(uint8_t z, uint32_t x, uint32_t y) {
    char b[64];
    snprintf(b, sizeof b, "%s/%u/%lu/%lu.swt", kMapsDir, z, (unsigned long)x, (unsigned long)y);
    return b;
}

bool tile_read(uint8_t z, uint32_t x, uint32_t y, std::vector<uint8_t>& out) {
    if (!mounted()) return false;
    if (!read_file(tile_path(z, x, y), out, 512 * 1024)) return false;
    std::lock_guard<std::mutex> g(mu);
    if (lru && lru->touch(block_key(z, x, y), ++access_clock)) lru_dirty = true;
    return true;
}

bool tile_write(uint8_t z, uint32_t x, uint32_t y, const std::vector<uint8_t>& data) {
    if (!mounted() || !lru) return false;
    {
        std::lock_guard<std::mutex> g(mu);
        // Category quota (4 GB, LRU with hysteresis) ...
        for (auto& k : lru->plan_eviction(data.size())) evict_block(k);
        // ... and the card-wide free-space reserve shared with music.
        if (inf.free < data.size() + ccore::kMinFreeReserve) {
            uint64_t need = data.size() + ccore::kMinFreeReserve - inf.free;
            for (auto& k : lru->plan_free_bytes(need)) evict_block(k);
            inf.free += need;  // refreshed from FatFs on the next usage poll
        }
    }
    char dir[48];
    snprintf(dir, sizeof dir, "%s/%u", kMapsDir, z);
    ensure_dir(dir);
    snprintf(dir, sizeof dir, "%s/%u/%lu", kMapsDir, z, (unsigned long)x);
    ensure_dir(dir);
    std::string p = tile_path(z, x, y);
    uint64_t old = file_size(p);
    if (!write_file_atomic(p, data.data(), data.size())) return false;
    std::lock_guard<std::mutex> g(mu);
    lru->add(block_key(z, x, y), static_cast<int64_t>(data.size()) - static_cast<int64_t>(old), ++access_clock);
    lru_dirty = true;
    if (inf.free > data.size()) inf.free -= data.size();
    return true;
}

bool write_file_atomic(const std::string& path, const uint8_t* data, size_t len) {
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(data, 1, len, f) == len;
    ok = (fflush(f) == 0) && ok;
    ok = (fsync(fileno(f)) == 0) && ok;
    fclose(f);
    if (!ok) {
        unlink(tmp.c_str());
        return false;
    }
    unlink(path.c_str());  // FAT rename does not replace an existing file
    if (rename(tmp.c_str(), path.c_str()) != 0) {
        unlink(tmp.c_str());
        return false;
    }
    return true;
}

bool read_file(const std::string& path, std::vector<uint8_t>& out, size_t max_len) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0 || static_cast<size_t>(n) > max_len) {
        fclose(f);
        return false;
    }
    out.resize(static_cast<size_t>(n));
    bool ok = fread(out.data(), 1, out.size(), f) == out.size();
    fclose(f);
    return ok;
}

uint64_t file_size(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0;
}

void flush_index() {
    std::vector<uint8_t> blob;
    {
        std::lock_guard<std::mutex> g(mu);
        int64_t now = esp_timer_get_time();
        // At most every 15 minutes: the index is advisory (rebuilt from the tree if lost),
        // so access-order updates are not written to the card on every tile read.
        if (!lru || !lru_dirty || now - lru_saved_us < 15LL * 60 * 1000000) return;
        lru_dirty = false;
        lru_saved_us = now;
        swp::Writer w(blob);
        w.u8(1);
        w.u64(access_clock);
        w.u32(static_cast<uint32_t>(lru->entries().size()));
        for (auto& e : lru->entries()) {
            w.str8(e.key);
            w.u64(e.size);
            w.u64(e.last_access);
        }
    }
    if (!write_file_atomic(kIndexPath, blob.data(), blob.size())) ESP_LOGW(TAG, "failed to persist map cache index");
}

}  // namespace storage
