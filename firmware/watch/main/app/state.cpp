#include "app/state.hpp"

#include <atomic>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace app {
namespace {
State* g_state = nullptr;
SemaphoreHandle_t g_mutex = nullptr;
std::atomic<uint32_t> g_dirty{0};
}  // namespace

const char* link_state_name(LinkState s) {
    switch (s) {
        case LinkState::NoPeer: return "Not paired";
        case LinkState::Disconnected: return "Disconnected";
        case LinkState::Waiting: return "Waiting to reconnect";
        case LinkState::Connecting: return "Connecting";
        case LinkState::Securing: return "Securing";
        case LinkState::Discovering: return "Discovering";
        case LinkState::Handshake: return "Handshake";
        case LinkState::Ready: return "Connected";
    }
    return "?";
}

void init() {
    if (g_state) return;
    g_mutex = xSemaphoreCreateRecursiveMutex();
    g_state = new State();  // lives for the whole program (PSRAM via malloc threshold)
}

Locked::Locked() : s_(g_state) { xSemaphoreTakeRecursive(g_mutex, portMAX_DELAY); }
Locked::~Locked() { xSemaphoreGiveRecursive(g_mutex); }

void mark(uint32_t bits) { g_dirty.fetch_or(bits); }
uint32_t take_dirty() { return g_dirty.exchange(0); }

void post_alert(const Alert& a) {
    {
        Locked s;
        if (s->alerts.size() < 8) s->alerts.push_back(a);
    }
    mark(kDirtyAlert);
}

}  // namespace app
