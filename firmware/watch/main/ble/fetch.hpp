// HTTPS requests from the watch, performed by the link peer (the companion; later a phone).
//
//   fetch::request({.url = "https://widgets.example.com/discord",
//                   .headers = {{"Authorization", "Bearer …"}}},
//                  [](fetch::Response&& r) { ... });
//
// The peer verifies TLS certificates and refuses plain http://. Credentials passed in
// headers travel over the encrypted BLE link and are visible to the peer.
// Callbacks run on the BLE host or esp_timer task: copy the result into app state and
// app::mark() a dirty flag instead of touching LVGL.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "swp/messages.hpp"

namespace fetch {

struct Request {
    swp::HttpMethod method = swp::HttpMethod::Get;
    std::string url;
    std::vector<swp::HttpHeader> headers;
    std::string body;
    uint32_t max_response = 16 * 1024;  // up to swp::kHttpMaxResponse
    uint32_t timeout_ms = 30000;
};

struct Response {
    uint16_t status = 0;  // 0: not performed (offline, timeout, transport error)
    std::string error;
    std::vector<uint8_t> body;
    bool ok() const { return status >= 200 && status < 300; }
    std::string text() const { return {body.begin(), body.end()}; }
};

using Callback = std::function<void(Response&&)>;

// False (and no callback) if the companion is not connected, the request is too large for
// one SWP frame, or too many requests are in flight.
bool request(Request req, Callback cb);

// Called by ble/link.cpp.
void init();
void on_response(swp::HttpResponse&& r);
void on_body(uint16_t id, std::vector<uint8_t>&& body);
void on_body_failed(uint16_t id);
void on_link_down();

}  // namespace fetch
