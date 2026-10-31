#include "ntfy.hpp"

#include <atomic>
#include <cstring>
#include <ctime>
#include <mutex>

#include "cJSON.h"
#include "ccore/backoff.hpp"
#include "ccore/ntfy.hpp"
#include "cloud.hpp"
#include "config.hpp"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.hpp"
#include "nvs.h"

namespace ntfy {
namespace {

constexpr const char* TAG = "ntfy";
constexpr const char* kNvsNs = "ntfy";
constexpr const char* kFirstSince = "10m";             // nothing acknowledged yet: recent history only
constexpr int kReadTimeoutMs = 2000;                    // bounds delivery latency of a partial read
constexpr int64_t kIdleReconnectUs = 120LL * 1000000;   // ntfy keepalive is ~45 s

std::mutex mu;
ccore::ntfy::AckCursor* cursor = nullptr;  // guarded by mu
std::atomic<uint32_t> last_event{0};
std::atomic<bool> is_connected{false};
std::atomic<bool> restart{false};
TaskHandle_t task = nullptr;

std::string load_cursor() {
    nvs_handle_t h;
    std::string out;
    if (nvs_open(kNvsNs, NVS_READONLY, &h) != ESP_OK) return out;
    char buf[64];
    size_t len = sizeof buf;
    if (nvs_get_str(h, "cursor", buf, &len) == ESP_OK) out = buf;
    nvs_close(h);
    return out;
}

void save_cursor(const std::string& c) {
    nvs_handle_t h;
    if (nvs_open(kNvsNs, NVS_READWRITE, &h) != ESP_OK) return;
    if (c.size() < 64) nvs_set_str(h, "cursor", c.c_str());
    nvs_commit(h);
    nvs_close(h);
}

std::string str(const cJSON* o, const char* k) {
    const cJSON* v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

void on_line(std::string_view line) {
    cJSON* j = cJSON_ParseWithLength(line.data(), line.size());
    if (!j) return;
    last_event.store(static_cast<uint32_t>(time(nullptr)));
    std::string event = str(j, "event");
    std::string id = str(j, "id");
    if (event == "message" && !id.empty() && id.size() < 64) {
        bool fresh;
        {
            std::lock_guard<std::mutex> g(mu);
            fresh = cursor->add(id);
        }
        if (fresh) {
            swp::Notification n;
            n.id = ccore::ntfy::message_key(id);
            const cJSON* t = cJSON_GetObjectItemCaseSensitive(j, "time");
            n.timestamp = cJSON_IsNumber(t) ? static_cast<uint32_t>(t->valuedouble) : static_cast<uint32_t>(time(nullptr));
            const cJSON* p = cJSON_GetObjectItemCaseSensitive(j, "priority");
            n.priority = static_cast<uint8_t>(cJSON_IsNumber(p) && p->valueint >= 1 && p->valueint <= 5 ? p->valueint : 3);
            n.topic = str(j, "topic").substr(0, 64);
            n.title = str(j, "title").substr(0, 128);
            n.body = str(j, "message").substr(0, 1024);
            ESP_LOGI(TAG, "message %s on %s", id.c_str(), n.topic.c_str());
            cloud::ingest_notification(n);
        }
    }
    cJSON_Delete(j);
}

// One subscription session; returns when the stream ends, fails or must restart.
bool stream_once(const std::string& url, const std::string& token) {
    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.method = HTTP_METHOD_GET;
    cfg.timeout_ms = kReadTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;  // certificate always verified
    cfg.buffer_size = 2048;
    cfg.buffer_size_tx = 1024;
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return false;
    esp_http_client_set_header(c, "User-Agent", "sw-companion/0.1");
    if (!token.empty()) esp_http_client_set_header(c, "Auth", token.c_str());
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0) {
        int status = esp_http_client_get_status_code(c);
        if (status == 200) {
            ok = true;
            is_connected.store(true);
            ESP_LOGI(TAG, "subscribed");
            ccore::ntfy::LineSplitter lines;
            char buf[512];
            int64_t last_rx = esp_timer_get_time();
            while (!restart.load() && net::online()) {
                int n = esp_http_client_read(c, buf, sizeof buf);
                if (n == -ESP_ERR_HTTP_EAGAIN || n == 0) {
                    if (n == 0 && esp_http_client_is_complete_data_received(c)) break;  // server closed the stream
                    if (esp_timer_get_time() - last_rx > kIdleReconnectUs) {
                        ESP_LOGW(TAG, "no keepalive for 120 s; reconnecting");
                        break;
                    }
                    continue;
                }
                if (n < 0) break;
                last_rx = esp_timer_get_time();
                lines.feed(buf, static_cast<size_t>(n), on_line);
            }
        } else {
            ESP_LOGW(TAG, "subscribe failed: HTTP %d", status);
        }
    } else {
        ESP_LOGW(TAG, "connect failed");
    }
    is_connected.store(false);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

void ntfy_task(void*) {
    ccore::Backoff backoff(5000, 300000, 20);
    for (;;) {
        restart.store(false);
        std::string base = config::get(config::kNtfyUrl);
        while (!base.empty() && base.back() == '/') base.pop_back();
        auto topics = ccore::ntfy::split_topics(config::get(config::kNtfyTopics));
        if (base.rfind("https://", 0) != 0 || topics.empty() || !net::online()) {
            // Not configured or offline: wait for a config change or connectivity.
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30000));
            continue;
        }
        std::string since;
        {
            std::lock_guard<std::mutex> g(mu);
            since = cursor->cursor().empty() ? kFirstSince : cursor->cursor();
        }
        std::string url = base + "/" + ccore::ntfy::join_topics(topics) + "/json?since=" + since;
        if (stream_once(url, config::get(config::kNtfyToken))) backoff.reset();
        if (restart.load()) continue;
        uint32_t wait_ms = backoff.next(esp_random());
        ESP_LOGI(TAG, "reconnecting in %lu ms", static_cast<unsigned long>(wait_ms));
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(wait_ms));  // cut short by connectivity / config changes
    }
}

void wake() {
    restart.store(true);
    if (task) xTaskNotifyGive(task);
}

void on_config(const std::string& key) {
    if (key == config::kNtfyUrl || key == config::kNtfyTopics || key == config::kNtfyToken) wake();
}

void on_net(bool online) {
    if (online && task) xTaskNotifyGive(task);  // subscribe immediately after connectivity returns
}

}  // namespace

void start() {
    cursor = new ccore::ntfy::AckCursor(load_cursor());
    config::add_listener(on_config);
    net::add_listener(on_net);
    xTaskCreatePinnedToCore(ntfy_task, "ntfy", 6144, nullptr, 4, &task, 0);
}

void on_ack(uint64_t key) {
    std::optional<std::string> adv;
    {
        std::lock_guard<std::mutex> g(mu);
        if (cursor) adv = cursor->ack(key);
    }
    if (adv) save_cursor(*adv);
}

uint32_t last_event_utc() { return last_event.load(); }
bool connected() { return is_connected.load(); }

}  // namespace ntfy
