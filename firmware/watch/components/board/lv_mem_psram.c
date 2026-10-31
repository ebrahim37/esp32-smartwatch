// LVGL allocator backed by PSRAM (CONFIG_LV_USE_CUSTOM_MALLOC). Widget trees, image caches
// and decoded map tiles live in the 8 MB PSRAM, leaving internal RAM for DMA, Bluetooth and
// task stacks. Falls back to internal RAM if PSRAM is exhausted.
#include <string.h>

#include "esp_heap_caps.h"
#include "lvgl.h"
#include "src/stdlib/lv_mem.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#define LV_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    (void)mem;
    (void)bytes;
    return NULL;
}
void lv_mem_remove_pool(lv_mem_pool_t pool) { (void)pool; }

void* lv_malloc_core(size_t size) {
    void* p = heap_caps_malloc(size, LV_CAPS);
    return p ? p : heap_caps_malloc(size, MALLOC_CAP_8BIT);
}

void* lv_realloc_core(void* p, size_t new_size) {
    void* r = heap_caps_realloc(p, new_size, LV_CAPS);
    return r ? r : heap_caps_realloc(p, new_size, MALLOC_CAP_8BIT);
}

void lv_free_core(void* p) { heap_caps_free(p); }

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    memset(mon_p, 0, sizeof(*mon_p));
    mon_p->total_size = heap_caps_get_total_size(LV_CAPS);
    mon_p->free_size = heap_caps_get_free_size(LV_CAPS);
    mon_p->free_biggest_size = heap_caps_get_largest_free_block(LV_CAPS);
    mon_p->used_pct = mon_p->total_size ? (uint8_t)(100 - (100 * mon_p->free_size) / mon_p->total_size) : 0;
}

lv_result_t lv_mem_test_core(void) { return heap_caps_check_integrity(LV_CAPS, true) ? LV_RESULT_OK : LV_RESULT_INVALID; }

#endif
