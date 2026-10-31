#include "web_config.hpp"

#include <string>
#include <vector>

#include "ble_server.hpp"
#include "config.hpp"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.hpp"

namespace web_config {
namespace {

constexpr const char* TAG = "webcfg";
constexpr size_t kMaxBody = 6 * 1024;  // all keys URL-encoded, with headroom
httpd_handle_t server = nullptr;

std::string html_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            case '\'': o += "&#39;"; break;
            default: o += c;
        }
    }
    return o;
}

int hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// application/x-www-form-urlencoded component; false on a malformed escape.
bool url_decode(const std::string& in, std::string& out) {
    out.clear();
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%') {
            if (i + 2 >= in.size()) return false;
            int h = hex(in[i + 1]), l = hex(in[i + 2]);
            if (h < 0 || l < 0) return false;
            out += static_cast<char>(h << 4 | l);
            i += 2;
        } else {
            out += in[i];
        }
    }
    return true;
}

std::string ip_string() {
    uint32_t ip = net::status().ipv4;
    if (!ip) return "";
    char b[16];
    snprintf(b, sizeof b, "%lu.%lu.%lu.%lu", (unsigned long)(ip & 0xFF), (unsigned long)(ip >> 8 & 0xFF),
             (unsigned long)(ip >> 16 & 0xFF), (unsigned long)(ip >> 24));
    return b;
}

std::string render(const std::string& message, bool error) {
    std::string h =
        "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content=\"width=device-width,initial-scale=1\">"
        "<title>SW Companion</title><style>"
        "body{font:16px system-ui,sans-serif;background:#000;color:#eee;margin:0 auto;padding:16px;max-width:560px}"
        "label{display:block;margin:14px 0 4px;color:#aaa;font-size:14px}"
        "input{box-sizing:border-box;width:100%;padding:10px;border:1px solid #333;border-radius:8px;background:#111;color:#eee;font:inherit}"
        "button{margin-top:20px;width:100%;padding:12px;border:0;border-radius:10px;background:#0a84ff;color:#fff;font:600 17px system-ui}"
        ".msg{padding:10px;border-radius:8px;background:#123;margin:12px 0}.err{background:#411}"
        "small{color:#777}</style></head><body><h2>SW Companion</h2>"
        "<p><small>Available while the pairing window is open (press BOOT to reopen). "
        "Secrets are never shown; leave them empty to keep the saved value.</small></p>";
    if (!message.empty()) h += "<div class=\"msg" + std::string(error ? " err" : "") + "\">" + html_escape(message) + "</div>";
    h += "<form method=post action=\"/\">";
    for (const auto& k : config::keys()) {
        std::string id = k.key;
        h += "<label for=\"" + id + "\">" + html_escape(k.description) + " <small>(" + id + ")</small></label>";
        if (k.secret) {
            h += "<input id=\"" + id + "\" name=\"" + id + "\" type=password autocomplete=new-password maxlength=" +
                 std::to_string(k.max_len) + " placeholder=\"" + (config::is_set(k.key) ? "saved" : "not set") + "\">";
        } else {
            h += "<input id=\"" + id + "\" name=\"" + id + "\" type=text autocomplete=off maxlength=" + std::to_string(k.max_len) +
                 " value=\"" + html_escape(config::get(k.key)) + "\">";
        }
    }
    h += "<button type=submit>Save</button></form></body></html>";
    return h;
}

esp_err_t send_page(httpd_req_t* req, const std::string& message, bool error) {
    std::string page = render(message, error);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    return httpd_resp_send(req, page.data(), page.size());
}

esp_err_t get_root(httpd_req_t* req) { return send_page(req, "", false); }

// Rejects cross-site form posts (another page in the same browser posting to this address).
bool same_origin(httpd_req_t* req) {
    char origin[96] = {}, host[64] = {};
    if (httpd_req_get_hdr_value_str(req, "Origin", origin, sizeof origin) != ESP_OK) return true;  // non-browser client
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof host) != ESP_OK) return false;
    return std::string(origin) == std::string("http://") + host;
}

esp_err_t post_root(httpd_req_t* req) {
    if (!same_origin(req)) {
        httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "cross-origin request");
        return ESP_FAIL;
    }
    if (req->content_len > kMaxBody) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "request too large");
        return ESP_FAIL;
    }
    std::string body(req->content_len, '\0');
    for (size_t got = 0; got < body.size();) {
        int r = httpd_req_recv(req, body.data() + got, body.size() - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) return ESP_FAIL;
        got += static_cast<size_t>(r);
    }

    int saved = 0;
    std::string failed;
    for (size_t pos = 0; pos <= body.size();) {
        size_t amp = body.find('&', pos);
        if (amp == std::string::npos) amp = body.size();
        std::string pair = body.substr(pos, amp - pos);
        pos = amp + 1;
        size_t eq = pair.find('=');
        if (pair.empty() || eq == std::string::npos) continue;
        std::string key, value;
        if (!url_decode(pair.substr(0, eq), key) || !url_decode(pair.substr(eq + 1), value)) {
            failed += failed.empty() ? "malformed form data" : ", malformed form data";
            continue;
        }
        const config::KeyInfo* k = config::find(key);
        if (!k) continue;
        if (k->secret ? value.empty() : value == config::get(k->key)) continue;  // unchanged
        if (config::set(key, value)) {
            ++saved;
            ESP_LOGI(TAG, "set %s", k->key);
        } else {
            failed += (failed.empty() ? "" : ", ") + key;
        }
    }
    if (!failed.empty()) return send_page(req, "Saved " + std::to_string(saved) + "; rejected: " + failed, true);
    return send_page(req, saved ? "Saved " + std::to_string(saved) + " setting(s)." : "Nothing changed.", false);
}

void start_server() {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = 80;
    cfg.ctrl_port = 32769;
    cfg.max_open_sockets = 3;
    cfg.lru_purge_enable = true;
    cfg.stack_size = 6144;
    cfg.core_id = 0;
    if (httpd_start(&server, &cfg) != ESP_OK) {
        server = nullptr;
        ESP_LOGW(TAG, "could not start HTTP server");
        return;
    }
    httpd_uri_t get = {.uri = "/", .method = HTTP_GET, .handler = get_root, .user_ctx = nullptr};
    httpd_uri_t post = {.uri = "/", .method = HTTP_POST, .handler = post_root, .user_ctx = nullptr};
    httpd_register_uri_handler(server, &get);
    httpd_register_uri_handler(server, &post);
}

void task(void*) {
    std::string announced;
    for (;;) {
        bool open = ble_server::pairing_window_open();
        if (open && !server) {
            start_server();
            announced.clear();
        } else if (!open && server) {
            httpd_stop(server);
            server = nullptr;
            ESP_LOGI(TAG, "config page closed (pairing window over)");
        }
        if (server) {
            std::string ip = ip_string();
            if (!ip.empty() && ip != announced) {
                ESP_LOGI(TAG, "config page: http://%s/ (until the pairing window closes)", ip.c_str());
                announced = ip;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

}  // namespace

void start() { xTaskCreatePinnedToCore(task, "webcfg", 3072, nullptr, 3, nullptr, 0); }

}  // namespace web_config
