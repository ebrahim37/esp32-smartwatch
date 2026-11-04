#include "music/memos.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include "ble_server.hpp"
#include "ccore/quota.hpp"
#include "ccore/sync_plan.hpp"
#include "esp_heap_caps.h"
#include "esp_hf_ag_api.h"
#include "esp_hf_ag_legacy_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "music/a2dp.hpp"
#include "music/player.hpp"
#include "storage.hpp"

namespace music::memos {
namespace {

constexpr const char* TAG = "memos";
constexpr int64_t kConnectTimeoutUs = 12 * 1000000LL;
constexpr int64_t kMaxLengthUs = 30 * 60 * 1000000LL;  // TODO(PROTOTYPE): fixed 30 min cap
constexpr size_t kStreamBytes = 64 * 1024;              // ~2 s at 16 kHz: rides out SD stalls
constexpr size_t kHeaderBytes = 44;

std::mutex mu;
swp::MemoState state = swp::MemoState::Idle;
std::string error;
uint8_t peer[6] = {};
int64_t started_us = 0;    // recording start (audio connected)
std::atomic<int64_t> ended_us{0};  // last recording/attempt ended
uint32_t sample_rate = 8000;

// Recording pipeline: HFP data callback (Bluetooth task) -> stream buffer (PSRAM) -> writer
// task -> SD file.
StreamBufferHandle_t stream = nullptr;
std::atomic<bool> writing{false}, stop_writer{false};
std::atomic<bool> slc_up{false};  // hands-free service link open (the headphones may open it themselves)
std::atomic<uint32_t> bytes_in{0};
std::string cur_path;
esp_timer_handle_t timer = nullptr;  // connect timeout / max length

void publish() { ble_server::to_watch(status()); }

void set_state(swp::MemoState s, const std::string& err = "") {
    {
        std::lock_guard<std::mutex> g(mu);
        bool was_busy = state == swp::MemoState::Connecting || state == swp::MemoState::Recording;
        if (was_busy && s != swp::MemoState::Connecting && s != swp::MemoState::Recording) ended_us.store(esp_timer_get_time());
        state = s;
        error = err;
    }
    publish();
}

void put_le(uint8_t* p, uint32_t v, int n) {
    for (int i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

void wav_header(uint8_t* h, uint32_t rate, uint32_t data_bytes) {
    memcpy(h, "RIFF", 4);
    put_le(h + 4, 36 + data_bytes, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le(h + 16, 16, 4);        // fmt chunk size
    put_le(h + 20, 1, 2);         // PCM
    put_le(h + 22, 1, 2);         // mono
    put_le(h + 24, rate, 4);
    put_le(h + 28, rate * 2, 4);  // byte rate
    put_le(h + 32, 2, 2);         // block align
    put_le(h + 34, 16, 2);        // bits per sample
    memcpy(h + 36, "data", 4);
    put_le(h + 40, data_bytes, 4);
}

// Drops every profile so the Bluetooth connection itself closes. Measured on AirPods: A2DP sent
// over a connection that has carried the hands-free audio link stays congested (stuttering,
// effectively silent) until the connection is recreated; playback reconnects fresh.
void release_headphones() {
    esp_hf_ag_vra_control(peer, ESP_HF_VR_STATE_DISABLED);
    esp_hf_ag_slc_disconnect(peer);
    a2dp::disconnect();
}

// Waits (bounded) for the A2DP link to close, so a memo played right away reconnects fresh.
void wait_released() {
    for (int i = 0; i < 60 && a2dp::connected(); ++i) vTaskDelay(pdMS_TO_TICKS(50));
}

// Gives up on a Start that hasn't reached recording yet.
void abort_connect() {
    esp_hf_ag_audio_disconnect(peer);
    release_headphones();
}

void writer_task(void*) {
    FILE* f = fopen(cur_path.c_str(), "wb");
    uint32_t data = 0;
    if (f) {
        uint8_t hdr[kHeaderBytes] = {};
        fwrite(hdr, 1, sizeof hdr, f);  // placeholder until the length is known
        uint8_t* buf = static_cast<uint8_t*>(heap_caps_malloc(4096, MALLOC_CAP_SPIRAM));
        int peak = 0;
        uint32_t level_bytes = 0;
        while (buf) {
            size_t n = xStreamBufferReceive(stream, buf, 4096, pdMS_TO_TICKS(100));
            if (n > 0 && fwrite(buf, 1, n, f) == n) data += n;
            // Diagnostics: peak level once a second (silent recordings).
            for (size_t i = 0; i + 1 < n; i += 2) peak = std::max(peak, std::abs(static_cast<int>(static_cast<int16_t>(buf[i] | buf[i + 1] << 8))));
            level_bytes += n;
            if (level_bytes >= sample_rate * 2) {
                ESP_LOGI(TAG, "level: peak %d of 32767", peak);
                peak = 0;
                level_bytes = 0;
            }
            if (n == 0 && stop_writer.load()) break;
        }
        heap_caps_free(buf);
        wav_header(hdr, sample_rate, data);
        fseek(f, 0, SEEK_SET);
        fwrite(hdr, 1, sizeof hdr, f);
        fclose(f);
        ESP_LOGI(TAG, "saved %s: %lu bytes (%lu ms at %lu Hz)", cur_path.c_str(), (unsigned long)data,
                 (unsigned long)(data / 2 * 1000ull / sample_rate), (unsigned long)sample_rate);
        if (data == 0) unlink(cur_path.c_str());  // nothing was heard: don't keep an empty memo
    } else {
        ESP_LOGE(TAG, "cannot create %s", cur_path.c_str());
    }
    writing.store(false);
    release_headphones();
    wait_released();
    set_state(f ? swp::MemoState::Idle : swp::MemoState::Error, f ? "" : "SD card not writable");
    ble_server::to_watch(list());
    vTaskDelete(nullptr);
}

// Bluetooth task: decoded microphone PCM (16-bit mono). Never blocks.
void on_audio_in(const uint8_t* buf, uint32_t len) {
    if (!writing.load() || stop_writer.load()) return;
    xStreamBufferSend(stream, buf, len, 0);
    bytes_in.fetch_add(len, std::memory_order_relaxed);
}

// Microphone only: nothing to send to the earpiece.
uint32_t on_audio_out(uint8_t*, uint32_t) { return 0; }

void start_writer(uint32_t rate) {
    sample_rate = rate;
    char name[32];
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    strftime(name, sizeof name, "%Y%m%d-%H%M%S", &lt);
    cur_path = std::string(storage::kMemosDir) + "/" + name + ".wav";
    xStreamBufferReset(stream);
    stop_writer.store(false);
    bytes_in.store(0);
    writing.store(true);
    if (xTaskCreatePinnedToCore(writer_task, "memo_wr", 3584, nullptr, 5, nullptr, 0) != pdPASS) {
        writing.store(false);
        esp_hf_ag_audio_disconnect(peer);
        release_headphones();
        set_state(swp::MemoState::Error, "out of memory");
        return;
    }
    {
        std::lock_guard<std::mutex> g(mu);
        started_us = esp_timer_get_time();
    }
    esp_timer_stop(timer);
    esp_timer_start_once(timer, kMaxLengthUs);
    esp_hf_ag_volume_control(peer, ESP_HF_VOLUME_CONTROL_TARGET_MIC, 15);  // full microphone gain
    ESP_LOGI(TAG, "recording %s at %lu Hz", cur_path.c_str(), (unsigned long)rate);
    set_state(swp::MemoState::Recording);
}

void finish() {
    esp_timer_stop(timer);
    if (writing.load()) stop_writer.store(true);  // the writer releases the headphones when done
}

// Voice recognition mode tells the headphones to route their microphone to us.
void open_mic() {
    esp_hf_ag_vra_control(peer, ESP_HF_VR_STATE_ENABLED);
    esp_hf_ag_audio_connect(peer);
}

void hf_cb(esp_hf_cb_event_t ev, esp_hf_cb_param_t* p) {
    switch (ev) {
        case ESP_HF_CONNECTION_STATE_EVT:
            ESP_LOGI(TAG, "hands-free connection state %d", p->conn_stat.state);
            slc_up.store(p->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED);
            if (p->conn_stat.state == ESP_HF_CONNECTION_STATE_SLC_CONNECTED && status().state == static_cast<uint8_t>(swp::MemoState::Connecting)) {
                open_mic();
            } else if (p->conn_stat.state == ESP_HF_CONNECTION_STATE_DISCONNECTED) {
                if (status().state == static_cast<uint8_t>(swp::MemoState::Connecting)) {
                    esp_timer_stop(timer);
                    set_state(swp::MemoState::Error, "Headphones didn't accept the microphone link");
                }
                finish();
            }
            break;
        case ESP_HF_AUDIO_STATE_EVT:
            ESP_LOGI(TAG, "audio state %d", p->audio_stat.state);
            if (p->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED || p->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC) {
                if (status().state == static_cast<uint8_t>(swp::MemoState::Connecting))
                    start_writer(p->audio_stat.state == ESP_HF_AUDIO_STATE_CONNECTED_MSBC ? 16000 : 8000);
            } else if (p->audio_stat.state == ESP_HF_AUDIO_STATE_DISCONNECTED) {
                finish();
            }
            break;
        // Minimal phone-like answers so the headphones complete the service-level connection.
        case ESP_HF_CIND_RESPONSE_EVT:
            esp_hf_ag_cind_response(p->cind_rep.remote_addr, ESP_HF_CALL_STATUS_NO_CALLS, ESP_HF_CALL_SETUP_STATUS_IDLE,
                                    ESP_HF_NETWORK_STATE_AVAILABLE, 4, ESP_HF_ROAMING_STATUS_INACTIVE, 4, ESP_HF_CALL_HELD_STATUS_NONE);
            break;
        case ESP_HF_COPS_RESPONSE_EVT:
            esp_hf_ag_cops_response(p->cops_rep.remote_addr, const_cast<char*>("SW"));
            break;
        case ESP_HF_CLCC_RESPONSE_EVT:
            // No calls: just the final OK.
            esp_hf_ag_clcc_response(p->clcc_rep.remote_addr, 0, ESP_HF_CURRENT_CALL_DIRECTION_OUTGOING, ESP_HF_CURRENT_CALL_STATUS_ACTIVE,
                                    ESP_HF_CURRENT_CALL_MODE_VOICE, ESP_HF_CURRENT_CALL_MPTY_TYPE_SINGLE, nullptr, ESP_HF_CALL_ADDR_TYPE_UNKNOWN);
            break;
        case ESP_HF_UNAT_RESPONSE_EVT:
            esp_hf_ag_unknown_at_send(p->unat_rep.remote_addr, nullptr);
            break;
        case ESP_HF_VOLUME_CONTROL_EVT:
            ESP_LOGI(TAG, "headphones report %s gain %d", p->volume_control.type == ESP_HF_VOLUME_TYPE_MIC ? "mic" : "speaker",
                     p->volume_control.volume);
            break;
        case ESP_HF_BVRA_RESPONSE_EVT:
            ESP_LOGI(TAG, "headphones voice recognition %d", p->vra_rep.value);
            // Headphone button / Siri gesture ends voice recognition: treat it as Stop.
            if (p->vra_rep.value == ESP_HF_VR_STATE_DISABLED && status().state == static_cast<uint8_t>(swp::MemoState::Recording))
                esp_hf_ag_audio_disconnect(peer);
            break;
        default:
            break;
    }
}

struct Entry {
    std::string name;
    uint32_t created = 0;
    uint32_t duration_ms = 0;
    uint64_t size = 0;
};

// Memos on the card, newest first (names are timestamps, so name order is time order).
std::vector<Entry> scan() {
    std::vector<Entry> out;
    DIR* d = opendir(storage::kMemosDir);
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() < 5 || n.compare(n.size() - 4, 4, ".wav") != 0) continue;
        std::string path = std::string(storage::kMemosDir) + "/" + n;
        if (writing.load() && path == cur_path) continue;  // still recording
        Entry m;
        m.name = n.substr(0, n.size() - 4);
        if (!ccore::safe_id(m.name)) continue;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        uint8_t h[kHeaderBytes];
        bool ok = fread(h, 1, sizeof h, f) == sizeof h && memcmp(h, "RIFF", 4) == 0;
        fseek(f, 0, SEEK_END);
        m.size = static_cast<uint64_t>(ftell(f));
        fclose(f);
        if (!ok) continue;
        uint32_t rate = h[24] | h[25] << 8 | h[26] << 16 | static_cast<uint32_t>(h[27]) << 24;
        if (rate) m.duration_ms = static_cast<uint32_t>((m.size - kHeaderBytes) / 2 * 1000ull / rate);
        struct tm lt = {};
        if (sscanf(m.name.c_str(), "%4d%2d%2d-%2d%2d%2d", &lt.tm_year, &lt.tm_mon, &lt.tm_mday, &lt.tm_hour, &lt.tm_min, &lt.tm_sec) == 6) {
            lt.tm_year -= 1900;
            lt.tm_mon -= 1;
            lt.tm_isdst = -1;
            m.created = static_cast<uint32_t>(mktime(&lt));
        }
        out.push_back(std::move(m));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name > b.name; });
    return out;
}

void start() {
    {
        std::lock_guard<std::mutex> g(mu);
        if (state == swp::MemoState::Connecting || state == swp::MemoState::Recording) return;
    }
    if (!storage::mounted()) return set_state(swp::MemoState::Error, "No SD card");
    uint64_t used = 0;
    for (auto& e : scan()) used += e.size;
    if (used >= ccore::kMemosQuota) return set_state(swp::MemoState::Error, "Memo storage full");
    auto a = a2dp::status();
    if (!a.remembered) return set_state(swp::MemoState::Error, "No headphones paired");
    memcpy(peer, a.bda, sizeof peer);
    // Music and the hands-free microphone can't share the headphones.
    if (player::active()) {
        swp::PlaybackCommand pause;
        pause.cmd = static_cast<uint8_t>(swp::PlaybackCmd::Pause);
        player::command(pause);
    }
    set_state(swp::MemoState::Connecting);
    esp_timer_stop(timer);
    esp_timer_start_once(timer, kConnectTimeoutUs);
    ESP_LOGI(TAG, "connecting hands-free to %02x:%02x:%02x:%02x:%02x:%02x", peer[0], peer[1], peer[2], peer[3], peer[4], peer[5]);
    if (slc_up.load()) {
        open_mic();  // the headphones already opened the hands-free link
    } else if (esp_hf_ag_slc_connect(peer) != ESP_OK) {
        esp_timer_stop(timer);
        set_state(swp::MemoState::Error, "Bluetooth busy");
    }
}

void stop() {
    auto s = static_cast<swp::MemoState>(status().state);
    if (s == swp::MemoState::Connecting) {
        esp_timer_stop(timer);
        abort_connect();
        set_state(swp::MemoState::Idle);
    } else if (s == swp::MemoState::Recording) {
        esp_hf_ag_audio_disconnect(peer);
        finish();
    }
}

void on_timer(void*) {
    auto s = static_cast<swp::MemoState>(status().state);
    if (s == swp::MemoState::Connecting) {
        ESP_LOGW(TAG, "headphones didn't open the microphone link in time");
        abort_connect();
        set_state(swp::MemoState::Error, "Headphones not available");
    } else if (s == swp::MemoState::Recording) {
        ESP_LOGI(TAG, "maximum memo length reached");
        stop();
    }
}

}  // namespace

void init() {
    stream = xStreamBufferCreateWithCaps(kStreamBytes, 1, MALLOC_CAP_SPIRAM);
    esp_timer_create_args_t ta = {};
    ta.callback = on_timer;
    ta.name = "memo";
    esp_timer_create(&ta, &timer);
    esp_hf_ag_register_callback(hf_cb);
    esp_hf_ag_init();
    esp_hf_ag_register_data_callback(on_audio_in, on_audio_out);
}

void command(const swp::MemoCommand& c) {
    switch (static_cast<swp::MemoOp>(c.op)) {
        case swp::MemoOp::Start: start(); break;
        case swp::MemoOp::Stop: stop(); break;
        case swp::MemoOp::List: ble_server::to_watch(list()); break;
        case swp::MemoOp::Delete:
            if (ccore::safe_id(c.name)) {
                std::string path = std::string(storage::kMemosDir) + "/" + c.name + ".wav";
                if (unlink(path.c_str()) == 0) ESP_LOGI(TAG, "deleted %s", path.c_str());
            }
            ble_server::to_watch(list());
            break;
    }
    publish();
}

swp::MemoStatus status() {
    std::lock_guard<std::mutex> g(mu);
    swp::MemoStatus s;
    s.state = static_cast<uint8_t>(state);
    s.error = error;
    if (state == swp::MemoState::Recording) s.elapsed_ms = static_cast<uint32_t>((esp_timer_get_time() - started_us) / 1000);
    return s;
}

swp::MemoList list() {
    swp::MemoList l;
    for (auto& e : scan()) {
        if (l.memos.size() >= 40) break;
        l.memos.push_back({e.name, e.created, e.duration_ms});
    }
    return l;
}

bool take_button() {
    auto st = static_cast<swp::MemoState>(status().state);
    if (st == swp::MemoState::Connecting || st == swp::MemoState::Recording) {
        ESP_LOGI(TAG, "headphone button: stopping the memo");
        stop();
        publish();
        return true;
    }
    return esp_timer_get_time() - ended_us.load() < 4000000;  // the press that stopped it, delivered late
}

uint16_t count() { return static_cast<uint16_t>(std::min<size_t>(scan().size(), 40)); }

bool track_at(uint16_t index, library::Track& out) {
    auto all = scan();
    if (index >= all.size()) return false;
    const Entry& e = all[index];
    out = library::Track{};
    out.id = e.name;
    char title[40];
    struct tm lt = {};
    time_t t = e.created;
    localtime_r(&t, &lt);
    strftime(title, sizeof title, "Memo %b %d, %H:%M", &lt);
    out.title = title;
    out.artist = "Voice memo";
    out.suffix = "wav";
    out.duration_s = std::max<uint32_t>(1, e.duration_ms / 1000);  // rounded down: the progress bar reaches the end
    out.size = e.size;
    out.available = true;
    out.path = std::string(storage::kMemosDir) + "/" + e.name + ".wav";
    return true;
}

}  // namespace music::memos
