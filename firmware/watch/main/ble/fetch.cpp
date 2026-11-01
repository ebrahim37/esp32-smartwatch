#include "ble/fetch.hpp"

#include <algorithm>
#include <map>
#include <mutex>

#include "ble/link.hpp"
#include "esp_timer.h"

namespace fetch {
namespace {

constexpr size_t kMaxInFlight = 4;

struct Pending {
    Callback cb;
    int64_t deadline_us = 0;
    uint32_t body_size = 0;  // > 0 once the HttpResponse announced a transferred body
    Response resp;
};

std::mutex mu;
std::map<uint16_t, Pending> pending;
uint16_t next_id = 1;
esp_timer_handle_t sweep_timer = nullptr;

// Removes `id` and runs its callback outside the lock.
void finish(uint16_t id, Response* r, const char* error) {
    Pending p;
    {
        std::lock_guard<std::mutex> g(mu);
        auto it = pending.find(id);
        if (it == pending.end()) return;
        p = std::move(it->second);
        pending.erase(it);
    }
    Response out = r ? std::move(*r) : std::move(p.resp);
    if (error) {
        out.status = 0;
        out.error = error;
        out.body.clear();
    }
    if (p.cb) p.cb(std::move(out));
}

void sweep(void*) {
    int64_t now = esp_timer_get_time();
    std::vector<uint16_t> late;
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& [id, p] : pending)
            if (now > p.deadline_us) late.push_back(id);
    }
    for (uint16_t id : late) finish(id, nullptr, "timeout");
}

}  // namespace

void init() {
    esp_timer_create_args_t a = {};
    a.callback = sweep;
    a.name = "fetch";
    esp_timer_create(&a, &sweep_timer);
    esp_timer_start_periodic(sweep_timer, 1000000);
}

bool request(Request req, Callback cb) {
    if (!swlink::ready() || req.url.size() > swp::kHttpMaxUrl || req.headers.size() > swp::kHttpMaxHeaders ||
        req.body.size() > swp::kHttpMaxRequestBody)
        return false;
    for (const auto& h : req.headers)
        if (h.name.size() > 255 || h.value.size() > 1024) return false;  // the encoder would truncate
    swp::HttpRequest m;
    m.method = static_cast<uint8_t>(req.method);
    m.url = std::move(req.url);
    m.headers = std::move(req.headers);
    m.body.assign(req.body.begin(), req.body.end());
    m.max_response = std::min(req.max_response, swp::kHttpMaxResponse);
    {
        std::lock_guard<std::mutex> g(mu);
        if (pending.size() >= kMaxInFlight) return false;
        m.id = next_id++;
        if (next_id == 0) next_id = 1;
    }
    std::vector<uint8_t> payload = swp::encode_payload(m);
    if (payload.size() > swp::kMaxPayload) return false;
    {
        std::lock_guard<std::mutex> g(mu);
        Pending& p = pending[m.id];
        p.cb = std::move(cb);
        p.deadline_us = esp_timer_get_time() + static_cast<int64_t>(req.timeout_ms) * 1000;
    }
    if (!swlink::enqueue(static_cast<uint8_t>(swp::MsgType::HttpRequest), std::move(payload))) {
        std::lock_guard<std::mutex> g(mu);
        pending.erase(m.id);
        return false;
    }
    return true;
}

void on_response(swp::HttpResponse&& r) {
    Response out;
    out.status = r.status;
    out.error = std::move(r.error);
    if (r.body_size > r.body.size()) {  // body follows as a transfer
        std::lock_guard<std::mutex> g(mu);
        auto it = pending.find(r.id);
        if (it == pending.end()) return;
        it->second.body_size = r.body_size;
        it->second.resp = std::move(out);
        return;
    }
    out.body = std::move(r.body);
    finish(r.id, &out, nullptr);
}

void on_body(uint16_t id, std::vector<uint8_t>&& body) {
    Response out;
    {
        std::lock_guard<std::mutex> g(mu);
        auto it = pending.find(id);
        if (it == pending.end() || !it->second.body_size) return;
        out = std::move(it->second.resp);
    }
    out.body = std::move(body);
    finish(id, &out, nullptr);
}

void on_body_failed(uint16_t id) { finish(id, nullptr, "body transfer failed"); }

void on_link_down() {
    std::vector<uint16_t> ids;
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& [id, p] : pending) ids.push_back(id);
    }
    for (uint16_t id : ids) finish(id, nullptr, "companion disconnected");
}

}  // namespace fetch
