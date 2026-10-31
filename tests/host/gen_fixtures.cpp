// Emits SWP frames encoded by the C++ reference implementation as JSON, so the TypeScript
// codec in web/ is tested against byte-identical fixtures.
#include <cstdio>
#include <string>
#include <vector>

#include "swp/messages.hpp"

static std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (uint8_t b : v) {
        s += d[b >> 4];
        s += d[b & 15];
    }
    return s;
}

int main() {
    std::vector<uint8_t> f;
    std::printf("{\n");
    swp::Hello h;
    h.role = 3;
    h.capabilities = 0x140;
    h.firmware = "web-0.1";
    h.device_name = "Bluefy";
    swp::encode_message(h, 1, f);
    std::printf("  \"hello\": \"%s\",\n", hex(f).c_str());
    swp::WifiConfigSet w;
    w.ssid = "HomeNet";
    w.password = "p\xC3\xA4ss";
    swp::encode_message(w, 2, f);
    std::printf("  \"wifi_config_set\": \"%s\",\n", hex(f).c_str());
    swp::WifiStatus ws;
    ws.state = 4;
    ws.provider = 1;
    ws.ssid = "HomeNet";
    ws.rssi = -61;
    ws.ipv4 = 0x0201A8C0;
    ws.password_set = 1;
    ws.internet_ok = 1;
    swp::encode_message(ws, 3, f);
    std::printf("  \"wifi_status\": \"%s\",\n", hex(f).c_str());
    swp::ConfigValue cv;
    cv.key = "srv_token";
    cv.is_set = 1;
    cv.secret = 1;
    swp::encode_message(cv, 4, f);
    std::printf("  \"config_value\": \"%s\",\n", hex(f).c_str());
    swp::WatchSettings st;
    swp::encode_message(st, 5, f);
    std::printf("  \"watch_settings\": \"%s\"\n", hex(f).c_str());
    std::printf("}\n");
}
