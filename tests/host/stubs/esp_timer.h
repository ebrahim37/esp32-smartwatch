// The fetch tests drive completion explicitly, without an ESP timer task.
#pragma once
#include <cstdint>

using esp_timer_handle_t = void*;
struct esp_timer_create_args_t {
    void (*callback)(void*);
    const char* name;
};
inline int64_t esp_timer_get_time() { return 1000; }
inline int esp_timer_create(const esp_timer_create_args_t*, esp_timer_handle_t* h) { *h = nullptr; return 0; }
inline int esp_timer_start_periodic(esp_timer_handle_t, uint64_t) { return 0; }
