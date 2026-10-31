#include "app/settings.hpp"

#include <cstdlib>
#include <ctime>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "swp/wire.hpp"

namespace settings {
namespace {

constexpr const char* TAG = "settings";
constexpr const char* kPart = "nvs_user";
constexpr const char* kNs = "watch";
SemaphoreHandle_t mu = nullptr;
swp::WatchSettings cached;

void apply_tz(const std::string& tz) {
    setenv("TZ", tz.empty() ? "UTC0" : tz.c_str(), 1);
    tzset();
}

}  // namespace

std::vector<uint8_t> blob(const char* key) {
    std::vector<uint8_t> out;
    nvs_handle_t h;
    if (nvs_open_from_partition(kPart, kNs, NVS_READONLY, &h) != ESP_OK) return out;
    size_t len = 0;
    if (nvs_get_blob(h, key, nullptr, &len) == ESP_OK && len > 0 && len < 16384) {
        out.resize(len);
        if (nvs_get_blob(h, key, out.data(), &len) != ESP_OK) out.clear();
    }
    nvs_close(h);
    return out;
}

void set_blob(const char* key, const std::vector<uint8_t>& data) {
    nvs_handle_t h;
    if (nvs_open_from_partition(kPart, kNs, NVS_READWRITE, &h) != ESP_OK) return;
    esp_err_t err = data.empty() ? nvs_erase_key(h, key) : nvs_set_blob(h, key, data.data(), data.size());
    if (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) nvs_commit(h);
    else ESP_LOGW(TAG, "write %s failed: %s", key, esp_err_to_name(err));
    nvs_close(h);
}

void init() {
    mu = xSemaphoreCreateMutex();
    esp_err_t err = nvs_flash_init_partition(kPart);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // Only reached on a layout/version change of this app-owned partition.
        ESP_LOGW(TAG, "re-initialising %s (%s)", kPart, esp_err_to_name(err));
        nvs_flash_erase_partition(kPart);
        err = nvs_flash_init_partition(kPart);
    }
    ESP_ERROR_CHECK(err);
    auto b = blob("settings");
    if (auto s = swp::decode_payload<swp::WatchSettings>(b); s && !b.empty()) cached = *s;
    apply_tz(cached.posix_tz);
}

swp::WatchSettings get() {
    xSemaphoreTake(mu, portMAX_DELAY);
    auto s = cached;
    xSemaphoreGive(mu);
    return s;
}

void set(const swp::WatchSettings& s) {
    xSemaphoreTake(mu, portMAX_DELAY);
    cached = s;
    xSemaphoreGive(mu);
    set_blob("settings", swp::encode_payload(s));
    apply_tz(s.posix_tz);
}

Peer peer() {
    Peer p;
    auto b = blob("peer");
    swp::Reader r(b.data(), b.size());
    if (b.size() < 8 || r.u8() != 1) return p;
    p.addr_type = r.u8();
    for (auto& x : p.addr) x = r.u8();
    p.name = r.str8();
    p.valid = !r.failed();
    return p;
}

void set_peer(const Peer& p) {
    std::vector<uint8_t> b;
    swp::Writer w(b);
    w.u8(1);
    w.u8(p.addr_type);
    w.raw(p.addr, 6);
    w.str8(p.name);
    set_blob("peer", b);
}

void clear_peer() { set_blob("peer", {}); }

std::vector<core::Alarm> alarms() {
    std::vector<core::Alarm> out;
    auto b = blob("alarms");
    swp::Reader r(b.data(), b.size());
    if (b.empty() || r.u8() != 1) return out;
    uint8_t n = r.u8();
    for (uint8_t i = 0; i < n && n <= 16 && !r.failed(); ++i) {
        core::Alarm a;
        a.id = r.u8();
        a.hour = r.u8() % 24;
        a.minute = r.u8() % 60;
        a.days = r.u8() & 0x7F;
        a.enabled = r.boolean();
        a.label = r.str8();
        out.push_back(a);
    }
    if (r.failed()) out.clear();
    return out;
}

void set_alarms(const std::vector<core::Alarm>& a) {
    std::vector<uint8_t> b;
    swp::Writer w(b);
    w.u8(1);
    w.u8(static_cast<uint8_t>(a.size()));
    for (auto& x : a) {
        w.u8(x.id);
        w.u8(x.hour);
        w.u8(x.minute);
        w.u8(x.days);
        w.boolean(x.enabled);
        w.str8(x.label);
    }
    set_blob("alarms", b);
}

StepCheckpoint steps() {
    StepCheckpoint c;
    auto b = blob("steps");
    swp::Reader r(b.data(), b.size());
    if (b.size() == 9 && r.u8() == 1) {
        c.steps = r.u32();
        c.day_key = r.i32();
    }
    return c;
}

void set_steps(const StepCheckpoint& c) {
    std::vector<uint8_t> b;
    swp::Writer w(b);
    w.u8(1);
    w.u32(c.steps);
    w.i32(c.day_key);
    set_blob("steps", b);
}

}  // namespace settings
