#include "http.hpp"

#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace http {
namespace {

constexpr const char* TAG = "http";

esp_http_client_handle_t open_client(const std::string& url, esp_http_client_method_t method, const Headers& headers) {
    if (url.rfind("https://", 0) != 0) return nullptr;  // TLS only
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = method;
    cfg.timeout_ms = 15000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    cfg.keep_alive_enable = false;
    cfg.disable_auto_redirect = false;
    cfg.max_redirection_count = 3;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return nullptr;
    esp_http_client_set_header(c, "User-Agent", "sw-companion/0.1");
    for (auto& h : headers) esp_http_client_set_header(c, h.first.c_str(), h.second.c_str());
    return c;
}

Result read_body(esp_http_client_handle_t c, std::vector<uint8_t>& body, size_t max_len) {
    Result r;
    int64_t len = esp_http_client_fetch_headers(c);
    r.status = esp_http_client_get_status_code(c);
    if (len < 0) {
        r.status = 0;
        r.error = "header fetch failed";
        return r;
    }
    if (len > 0 && static_cast<size_t>(len) > max_len) {
        r.status = 0;
        r.error = "response too large";
        return r;
    }
    body.clear();
    if (len > 0) body.reserve(static_cast<size_t>(len));
    uint8_t buf[1024];
    for (;;) {
        int n = esp_http_client_read(c, reinterpret_cast<char*>(buf), sizeof buf);
        if (n < 0) {
            r.status = 0;
            r.error = "read failed";
            return r;
        }
        if (n == 0) break;
        if (body.size() + n > max_len) {
            r.status = 0;
            r.error = "response too large";
            return r;
        }
        body.insert(body.end(), buf, buf + n);
    }
    return r;
}

struct Sink {
    std::vector<uint8_t>* body;
    size_t max_len;
    bool too_large;
};

esp_err_t sink_event(esp_http_client_event_t* e) {
    auto* s = static_cast<Sink*>(e->user_data);
    if (e->event_id == HTTP_EVENT_ON_DATA && s && !s->too_large) {
        if (s->body->size() + e->data_len > s->max_len) s->too_large = true;
        else s->body->insert(s->body->end(), static_cast<uint8_t*>(e->data), static_cast<uint8_t*>(e->data) + e->data_len);
    }
    return ESP_OK;
}

std::string host_of(const std::string& url) {
    size_t a = url.find("://");
    if (a == std::string::npos) return "";
    a += 3;
    return url.substr(a, url.find('/', a) - a);
}

}  // namespace

Session::~Session() { reset(); }

void Session::reset() {
    if (client_) esp_http_client_cleanup(client_);
    client_ = nullptr;
    host_.clear();
}

Result Session::get(const std::string& url, const Headers& headers, std::vector<uint8_t>& body, size_t max_len) {
    Result r;
    if (url.rfind("https://", 0) != 0) {
        r.error = "invalid/non-TLS url";
        return r;
    }
    Sink sink{&body, max_len, false};
    for (int attempt = 0; attempt < 2; ++attempt) {  // a kept-alive connection may have been closed by the server
        if (client_ && host_of(url) != host_) reset();
        if (!client_) {
            esp_http_client_config_t cfg = {};
            cfg.url = url.c_str();
            cfg.timeout_ms = 15000;
            cfg.crt_bundle_attach = esp_crt_bundle_attach;
            cfg.buffer_size = 4096;
            cfg.buffer_size_tx = 1024;
            cfg.keep_alive_enable = true;
            cfg.event_handler = sink_event;
            client_ = esp_http_client_init(&cfg);
            if (!client_) {
                r.error = "client init failed";
                return r;
            }
            host_ = host_of(url);
            esp_http_client_set_header(client_, "User-Agent", "sw-companion/0.1");
        } else {
            esp_http_client_set_url(client_, url.c_str());
        }
        for (auto& h : headers) esp_http_client_set_header(client_, h.first.c_str(), h.second.c_str());
        body.clear();
        sink.too_large = false;
        esp_http_client_set_user_data(client_, &sink);
        esp_err_t err = esp_http_client_perform(client_);
        if (err == ESP_OK) {
            r.status = esp_http_client_get_status_code(client_);
            if (sink.too_large) {
                r.status = 0;
                r.error = "response too large";
            }
            return r;
        }
        r.error = esp_err_to_name(err);
        reset();
    }
    ESP_LOGW(TAG, "GET failed: %s", r.error.c_str());
    return r;
}

Result get(const std::string& url, const Headers& headers, std::vector<uint8_t>& body, size_t max_len) {
    Result r;
    esp_http_client_handle_t c = open_client(url, HTTP_METHOD_GET, headers);
    if (!c) {
        r.error = "invalid/non-TLS url";
        return r;
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        r.error = esp_err_to_name(err);
    } else {
        r = read_body(c, body, max_len);
    }
    esp_http_client_cleanup(c);
    if (!r.ok()) ESP_LOGW(TAG, "GET failed: status %d %s", r.status, r.error.c_str());
    return r;
}

Result request(esp_http_client_method_t method, const std::string& url, const Headers& headers, const std::vector<uint8_t>& req_body,
               std::vector<uint8_t>& body, size_t max_len) {
    Result r;
    esp_http_client_handle_t c = open_client(url, method, headers);
    if (!c) {
        r.error = "invalid/non-TLS url";
        return r;
    }
    int n = static_cast<int>(req_body.size());
    esp_err_t err = esp_http_client_open(c, n);
    if (err == ESP_OK && (n == 0 || esp_http_client_write(c, reinterpret_cast<const char*>(req_body.data()), n) == n))
        r = read_body(c, body, max_len);
    else
        r.error = err != ESP_OK ? esp_err_to_name(err) : "write failed";
    esp_http_client_cleanup(c);
    return r;
}

Result post_json(const std::string& url, const Headers& headers, const std::string& json, std::vector<uint8_t>& body, size_t max_len) {
    Result r;
    esp_http_client_handle_t c = open_client(url, HTTP_METHOD_POST, headers);
    if (!c) {
        r.error = "invalid/non-TLS url";
        return r;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_err_t err = esp_http_client_open(c, static_cast<int>(json.size()));
    if (err == ESP_OK && esp_http_client_write(c, json.data(), static_cast<int>(json.size())) == static_cast<int>(json.size()))
        r = read_body(c, body, max_len);
    else
        r.error = esp_err_to_name(err);
    esp_http_client_cleanup(c);
    return r;
}

Result download(const std::string& url, const Headers& headers, const std::string& path, uint64_t resume_from, uint64_t expected_size,
                const std::function<bool()>& should_abort, uint64_t* bytes_written) {
    Result r;
    esp_http_client_handle_t c = open_client(url, HTTP_METHOD_GET, headers);
    if (!c) {
        r.error = "invalid/non-TLS url";
        return r;
    }
    if (resume_from > 0) {
        char range[48];
        snprintf(range, sizeof range, "bytes=%llu-", static_cast<unsigned long long>(resume_from));
        esp_http_client_set_header(c, "Range", range);
    }
    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        r.error = esp_err_to_name(err);
        esp_http_client_cleanup(c);
        return r;
    }
    int64_t len = esp_http_client_fetch_headers(c);
    r.status = esp_http_client_get_status_code(c);
    bool append = resume_from > 0 && r.status == 206;
    if (!(r.status == 200 || r.status == 206) || len < 0) {
        if (r.error.empty()) r.error = "bad status";
        esp_http_client_cleanup(c);
        return r;
    }
    uint64_t base = append ? resume_from : 0;
    if (expected_size > 0 && len > 0 && base + static_cast<uint64_t>(len) != expected_size) {
        // Size disagrees with the playlist metadata: refuse rather than store a wrong file.
        r.status = 0;
        r.error = "size mismatch";
        esp_http_client_cleanup(c);
        return r;
    }
    FILE* f = fopen(path.c_str(), append ? "ab" : "wb");
    if (!f) {
        r.status = 0;
        r.error = "cannot open file";
        esp_http_client_cleanup(c);
        return r;
    }
    static constexpr size_t kBuf = 8192;
    auto* buf = static_cast<char*>(malloc(kBuf));
    uint64_t written = base;
    int64_t t0 = esp_timer_get_time();
    while (buf) {
        if (should_abort && should_abort()) {
            r.status = 0;
            r.error = "aborted";
            break;
        }
        int n = esp_http_client_read(c, buf, kBuf);
        if (n < 0) {
            r.status = 0;
            r.error = "read failed";
            break;
        }
        if (n == 0) break;
        if (fwrite(buf, 1, n, f) != static_cast<size_t>(n)) {
            r.status = 0;
            r.error = "SD write failed";
            break;
        }
        written += n;
        if ((written - base) / (512 * 1024) != (written - base - n) / (512 * 1024))
            ESP_LOGI(TAG, "download: %llu KB in %lld ms", (unsigned long long)((written - base) / 1024), (esp_timer_get_time() - t0) / 1000);
        taskYIELD();
    }
    free(buf);
    fflush(f);
    fclose(f);
    if (r.ok() && !esp_http_client_is_complete_data_received(c) && len > 0) {
        r.status = 0;
        r.error = "truncated";
    }
    esp_http_client_cleanup(c);
    if (bytes_written) *bytes_written = written;
    return r;
}

}  // namespace http
