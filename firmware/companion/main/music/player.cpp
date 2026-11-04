#include "music/player.hpp"

#include <atomic>
#include <type_traits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "ble_server.hpp"
#include "ccore/pcm.hpp"
#include "ccore/play_queue.hpp"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "music/a2dp.hpp"
#include "music/library.hpp"
#include "music/memos.hpp"
#include "storage.hpp"

namespace music::player {
namespace {

constexpr const char* TAG = "player";
constexpr size_t kRingFrames = 32768;  // power of two; 0.74 s of 44.1 kHz stereo
constexpr size_t kInBuf = 4096;
constexpr uint32_t kRate = ccore::PcmConverter::kOutRate;

// ---- SPSC PCM ring (producer: decoder task, consumer: Bluetooth A2DP task) -------------
int16_t* ring = nullptr;  // stereo frames
std::atomic<size_t> r_head{0}, r_tail{0};  // head: write index, tail: read index (frames)
size_t ring_used() { return r_head.load(std::memory_order_acquire) - r_tail.load(std::memory_order_acquire); }
size_t ring_free() { return kRingFrames - ring_used(); }
void ring_flush() { r_tail.store(r_head.load()); }

std::atomic<bool> playing{false};      // user intent: play
std::atomic<uint64_t> played_frames{0};
std::atomic<uint32_t> alert_left{0};   // frames of alert tone still to mix
std::atomic<uint32_t> alert_phase{0};

int32_t pcm_source(uint8_t* buf, int32_t len) {
    auto* out = reinterpret_cast<int16_t*>(buf);
    size_t want = static_cast<size_t>(len) / 4;
    size_t got = 0;
    if (playing.load(std::memory_order_relaxed)) {
        size_t avail = ring_used();
        got = want < avail ? want : avail;
        size_t t = r_tail.load(std::memory_order_relaxed);
        for (size_t i = 0; i < got; ++i) {
            size_t idx = ((t + i) & (kRingFrames - 1)) * 2;
            out[2 * i] = ring[idx];
            out[2 * i + 1] = ring[idx + 1];
        }
        r_tail.store(t + got, std::memory_order_release);
        played_frames.fetch_add(got, std::memory_order_relaxed);
    }
    if (got < want) memset(out + 2 * got, 0, (want - got) * 4);  // underrun -> silence
    uint32_t left = alert_left.load(std::memory_order_relaxed);
    if (left) {
        // 880 Hz beeps, 400 ms on / 200 ms off, mixed at about -9 dBFS.
        uint32_t ph = alert_phase.load(std::memory_order_relaxed);
        size_t n = want < left ? want : left;
        for (size_t i = 0; i < n; ++i, ++ph) {
            bool on = (ph % 26460) < 17640;
            int32_t s = on ? static_cast<int32_t>(11000 * std::sin(2 * M_PI * 880.0 * ph / kRate)) : 0;
            for (int c = 0; c < 2; ++c) {
                int32_t v = out[2 * i + c] / 2 + s;
                out[2 * i + c] = static_cast<int16_t>(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
            }
        }
        alert_phase.store(ph, std::memory_order_relaxed);
        alert_left.store(left - static_cast<uint32_t>(n), std::memory_order_relaxed);
    }
    if (!a2dp::remote_volume()) {
        // Headphones without absolute volume: apply it here (squared for a perceptual curve).
        int32_t v = a2dp::volume();
        int32_t g = v * v * 32768 / (127 * 127);
        if (g < 32768)
            for (size_t i = 0; i < want * 2; ++i) out[i] = static_cast<int16_t>(out[i] * g / 32768);
    }
    return len;
}

// ---- Player state (decoder task) ---------------------------------------------------------
enum class Op : uint8_t { Command, Remote };
// FreeRTOS queues copy items bytewise, so the queued command must be plain data (a
// std::string member would arrive as a dangling pointer and corrupt the heap).
struct Msg {
    uint8_t cmd;
    uint32_t arg;
    char str[64];  // playlist id
    int64_t queued_us;
};
static_assert(std::is_trivially_copyable_v<Msg>);
QueueHandle_t q;

std::mutex mu;
swp::PlaybackStatus st;
ccore::PlayQueue order;
std::string playlist;
library::Track cur;

FILE* file = nullptr;
char* file_buf = nullptr;
esp_audio_simple_dec_handle_t dec = nullptr;
ccore::PcmConverter conv;
bool conv_ready = false;
uint8_t* inbuf = nullptr;
size_t in_len = 0;
uint8_t* outbuf = nullptr;
uint32_t outbuf_cap = 0;
int16_t* convbuf = nullptr;
constexpr size_t kConvFrames = 2048;
uint64_t skip_until_ms = 0;  // decode-and-discard target for seeks without byte seeking
bool eos = false;
bool decoding = false;
int64_t last_publish_us = 0;

// Track `i` of the current playlist: the music library, or the voice memos.
bool lookup(uint16_t i, library::Track& out) {
    return playlist == memos::kPlaylistId ? memos::track_at(i, out) : library::track_at(playlist, i, out);
}

void publish() {
    swp::PlaybackStatus s;
    std::vector<swp::UpNextItem> up;
    for (uint16_t i : order.upcoming(4)) {
        library::Track t;
        if (lookup(i, t)) up.push_back({t.title, t.artist});
    }
    {
        std::lock_guard<std::mutex> g(mu);
        st.position_ms = static_cast<uint32_t>(played_frames.load() * 1000 / kRate);
        st.volume = a2dp::volume();
        st.sink_connected = a2dp::connected();
        st.shuffle = order.shuffle();
        st.repeat = static_cast<uint8_t>(order.repeat());
        st.up_next = std::move(up);
        s = st;
    }
    last_publish_us = esp_timer_get_time();
    ble_server::to_watch(s);
}

void set_state(swp::PlayState s, const std::string& err = "") {
    {
        std::lock_guard<std::mutex> g(mu);
        st.state = static_cast<uint8_t>(s);
        st.error = err;
    }
    publish();
}

esp_audio_simple_dec_type_t type_for(const std::string& suffix) {
    if (suffix == "mp3") return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    if (suffix == "flac") return ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC;
    if (suffix == "aac") return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    if (suffix == "m4a" || suffix == "mp4") return ESP_AUDIO_SIMPLE_DEC_TYPE_M4A;  // AAC, ALAC or MP3 in MP4
    if (suffix == "wav") return ESP_AUDIO_SIMPLE_DEC_TYPE_WAV;                      // voice memos
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

void close_track() {
    if (dec) esp_audio_simple_dec_close(dec);
    dec = nullptr;
    if (file) fclose(file);
    file = nullptr;
    in_len = 0;
    conv_ready = false;
    decoding = false;
    eos = false;
}

bool open_decoder() {
    esp_audio_simple_dec_cfg_t cfg = {};
    cfg.dec_type = type_for(cur.suffix);
    if (cfg.dec_type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) return false;
    return esp_audio_simple_dec_open(&cfg, &dec) == ESP_AUDIO_ERR_OK;
}

// Opens the track at the queue's current position; `start_ms` > 0 seeks.
bool open_track(uint32_t start_ms) {
    close_track();
    ring_flush();
    int idx = order.current();
    if (idx < 0 || !lookup(static_cast<uint16_t>(idx), cur)) return false;
    if (!cur.available) {
        ESP_LOGW(TAG, "track %s not downloaded yet", cur.id.c_str());
        return false;
    }
    std::string path = cur.path.empty() ? library::path_for(cur.id, cur.suffix) : cur.path;
    file = fopen(path.c_str(), "rb");
    if (!file) return false;
    // Large stdio buffer in PSRAM: fewer, bigger SD reads.
    setvbuf(file, file_buf, _IOFBF, 32 * 1024);
    if (!open_decoder()) {
        close_track();
        return false;
    }
    skip_until_ms = 0;
    uint32_t dur_ms = cur.duration_s * 1000;
    // Subsonic durations are whole seconds. Our 128 kbps CBR MP3s give the exact length from the
    // file size (8 bits / 128 kbit/s = 1 ms per 16 bytes); used when it agrees with the server
    // (an original passed through untranscoded has a different bitrate).
    if (cur.suffix == "mp3" && cur.size > 0) {
        uint32_t est = static_cast<uint32_t>(cur.size / 16);
        if (est + 3000 > dur_ms && est < dur_ms + 3000) dur_ms = est;
    }
    if (start_ms > 0 && dur_ms > 0) {
        if (cur.suffix == "mp3") {
            // MP3 frames are self-synchronising: jump to the proportional byte offset.
            long off = static_cast<long>(static_cast<double>(start_ms) / dur_ms * cur.size);
            fseek(file, off, SEEK_SET);
        } else {
            // TODO(PROTOTYPE): FLAC/M4A seek by decoding and discarding up to the target.
            // Precise seeking needs FLAC SEEKTABLE / MP4 sample-table parsing.
            skip_until_ms = start_ms;
        }
    }
    played_frames.store(static_cast<uint64_t>(start_ms) * kRate / 1000);
    {
        std::lock_guard<std::mutex> g(mu);
        st.playlist_id = playlist;
        st.index = static_cast<uint16_t>(idx);
        st.track_id = cur.id;
        st.title = cur.title;
        st.artist = cur.artist;
        st.album = cur.album;
        st.duration_ms = dur_ms;
    }
    decoding = true;
    ESP_LOGI(TAG, "playing %s - %s (%s)", cur.artist.c_str(), cur.title.c_str(), cur.suffix.c_str());
    return true;
}

// Advances to the next playable track. Returns false at the end of the queue.
bool advance(bool user) {
    if (!user && playlist == memos::kPlaylistId) return false;  // a memo plays on its own, then stops
    for (uint16_t tries = 0; tries <= order.size(); ++tries) {
        if (!order.next(user)) return false;
        if (open_track(0)) {
            if (user || order.repeat() != ccore::Repeat::One) order.reshuffle_upcoming(esp_random());
            return true;
        }
        user = true;  // skip unavailable tracks even in repeat-one
    }
    return false;
}

void push_pcm(const uint8_t* pcm, size_t bytes) {
    while (bytes > 0 && decoding) {
        size_t consumed = 0;
        size_t n = conv.process(pcm, bytes, convbuf, kConvFrames, consumed);
        pcm += consumed;
        bytes -= consumed;
        if (skip_until_ms) {
            uint64_t pos_ms = played_frames.load() * 1000 / kRate;
            played_frames.fetch_add(n);
            if (pos_ms >= skip_until_ms) skip_until_ms = 0;
            if (consumed == 0) break;
            continue;
        }
        size_t off = 0;
        while (off < n) {
            // Wait for space; the A2DP task drains ~44 frames/ms.
            while (ring_free() < 256) {
                if (!playing.load()) return;
                if (!a2dp::connected()) {  // headphones gone: nothing drains the ring (the task pauses)
                    vTaskDelay(pdMS_TO_TICKS(50));
                    return;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            size_t h = r_head.load(std::memory_order_relaxed);
            size_t k = std::min(n - off, ring_free());
            for (size_t i = 0; i < k; ++i) {
                size_t idx = ((h + i) & (kRingFrames - 1)) * 2;
                ring[idx] = convbuf[2 * (off + i)];
                ring[idx + 1] = convbuf[2 * (off + i) + 1];
            }
            r_head.store(h + k, std::memory_order_release);
            off += k;
        }
        if (consumed == 0 && n == 0) break;
    }
}

// One decode step: read a block, decode it, push PCM. Returns false at end of stream.
bool decode_step() {
    if (!eos && in_len < kInBuf) {
        size_t n = fread(inbuf + in_len, 1, kInBuf - in_len, file);
        in_len += n;
        if (n == 0) eos = feof(file) || ferror(file);
    }
    esp_audio_simple_dec_raw_t raw = {};
    raw.buffer = inbuf;
    raw.len = static_cast<uint32_t>(in_len);
    raw.eos = eos;
    bool produced = false;
    while (raw.len > 0 || eos) {
        esp_audio_simple_dec_out_t out = {};
        out.buffer = outbuf;
        out.len = outbuf_cap;
        esp_audio_err_t err = esp_audio_simple_dec_process(dec, &raw, &out);
        if (err == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            uint8_t* nb = static_cast<uint8_t*>(heap_caps_realloc(outbuf, out.needed_size, MALLOC_CAP_SPIRAM));
            if (!nb) return false;
            outbuf = nb;
            outbuf_cap = out.needed_size;
            continue;
        }
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "decode error %d", err);
            return false;
        }
        if (out.decoded_size > 0) {
            produced = true;
            if (!conv_ready) {
                esp_audio_simple_dec_info_t info;
                if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK && conv.configure(info.sample_rate, info.channel, info.bits_per_sample)) {
                    conv_ready = true;
                    ESP_LOGI(TAG, "stream: %lu Hz, %u ch, %u bit", (unsigned long)info.sample_rate, info.channel, info.bits_per_sample);
                } else {
                    ESP_LOGE(TAG, "unsupported PCM format");
                    return false;
                }
            }
            push_pcm(out.buffer, out.decoded_size);
        }
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
        if (raw.consumed == 0 && out.decoded_size == 0) break;  // needs more input
        if (!decoding || !playing.load()) break;
    }
    // Keep unconsumed bytes for the next read.
    size_t used = in_len - raw.len;
    if (used > 0) memmove(inbuf, inbuf + used, raw.len);
    in_len = raw.len;
    // At end of file the decoder may leave a partial block it will never consume (seen with WAV):
    // a step that neither consumes nor produces ends the track.
    return !(eos && !produced && (in_len == 0 || used == 0));
}

void handle(const swp::PlaybackCommand& c) {
    using swp::PlaybackCmd;
    ESP_LOGI(TAG, "command %u (arg %u%s%s)", static_cast<unsigned>(c.cmd), static_cast<unsigned>(c.arg), c.str.empty() ? "" : ", ",
             c.str.c_str());
    switch (static_cast<PlaybackCmd>(c.cmd)) {
        case PlaybackCmd::PlayPlaylist: {
            playlist = c.str.empty() ? library::kAllSongsId : c.str;
            uint16_t n = playlist == memos::kPlaylistId ? memos::count() : library::size(playlist);
            if (n == 0) return set_state(swp::PlayState::Error, "playlist not synced");
            // arg UINT32_MAX: start from a random track (Shuffle buttons).
            uint16_t start = c.arg == UINT32_MAX ? static_cast<uint16_t>(esp_random() % n) : static_cast<uint16_t>(c.arg);
            order.load(n, start, order.shuffle(), esp_random());
            if (!a2dp::ensure_connected(8000)) return set_state(swp::PlayState::Error, "headphones not available");
            if (!open_track(0) && !advance(true)) return set_state(swp::PlayState::Error, "no downloaded tracks");
            playing.store(true);
            a2dp::start_stream();
            set_state(swp::PlayState::Playing);
            break;
        }
        case PlaybackCmd::Toggle:
            return handle({static_cast<uint8_t>(playing.load() ? PlaybackCmd::Pause : PlaybackCmd::Resume), 0, ""});
        case PlaybackCmd::Pause:
            if (!playing.exchange(false)) return;
            a2dp::suspend_stream();
            set_state(swp::PlayState::Paused);
            break;
        case PlaybackCmd::Resume:
            if (!file) {
                if (!playlist.empty()) return handle({static_cast<uint8_t>(PlaybackCmd::PlayPlaylist), st.index, playlist});
                // Nothing loaded (e.g. after a reboot): shuffle All Songs from a random track.
                uint16_t n = library::size(library::kAllSongsId);
                if (n == 0) return set_state(swp::PlayState::Error, "no songs downloaded yet");
                order.set_shuffle(true, esp_random());
                order.set_repeat(ccore::Repeat::All);
                return handle({static_cast<uint8_t>(PlaybackCmd::PlayPlaylist), UINT32_MAX, library::kAllSongsId});
            }
            if (!a2dp::ensure_connected(8000)) return set_state(swp::PlayState::Error, "headphones not available");
            playing.store(true);
            a2dp::start_stream();
            set_state(swp::PlayState::Playing);
            break;
        case PlaybackCmd::Next:
            if (!advance(true)) {
                playing.store(false);
                close_track();
                a2dp::suspend_stream();
                return set_state(swp::PlayState::Stopped);
            }
            publish();
            break;
        case PlaybackCmd::Previous: {
            bool restart = played_frames.load() > 3ull * kRate;
            order.prev(restart);
            if (!open_track(0)) advance(true);
            else if (!restart) order.reshuffle_upcoming(esp_random());
            publish();
            break;
        }
        case PlaybackCmd::SkipTo: {
            if (c.arg < 1 || c.arg > 4 || order.empty()) break;
            for (uint32_t k = 1; k < c.arg; ++k) order.next(true);
            if (!advance(true)) {
                playing.store(false);
                close_track();
                a2dp::suspend_stream();
                return set_state(swp::PlayState::Stopped);
            }
            publish();
            break;
        }
        case PlaybackCmd::Seek:
            if (file) {
                open_track(std::min<uint32_t>(c.arg, cur.duration_s * 1000));
                publish();
            }
            break;
        case PlaybackCmd::SetShuffle:
            order.set_shuffle(c.arg != 0, esp_random());
            publish();
            break;
        case PlaybackCmd::SetRepeat:
            order.set_repeat(static_cast<ccore::Repeat>(std::min<uint32_t>(c.arg, 2)));
            publish();
            break;
        case PlaybackCmd::Stop:
            playing.store(false);
            close_track();
            ring_flush();
            a2dp::suspend_stream();
            set_state(swp::PlayState::Stopped);
            break;
        case PlaybackCmd::SetVolume:
            a2dp::set_volume(static_cast<uint8_t>(c.arg));
            publish();
            break;
        case PlaybackCmd::AlertTone:
            alert_tone(static_cast<uint8_t>(c.arg ? c.arg : 3));
            break;
    }
}

int64_t start_grace_us = 0;  // last PlayPlaylist/Resume: the link may still be coming up

void task(void*) {
    Msg m;
    for (;;) {
        if (playing.load() && !a2dp::connected() && esp_timer_get_time() - start_grace_us > 10000000) {
            // The headphones disconnected mid-playback (taken by the phone, out of range).
            ESP_LOGW(TAG, "headphones disconnected: pausing");
            playing.store(false);
            set_state(swp::PlayState::Paused);
        }
        bool busy = decoding && playing.load();
        if (xQueueReceive(q, &m, busy ? 0 : pdMS_TO_TICKS(500)) == pdTRUE) {
            // Starting playback can wait seconds for the headphones to connect; a play/pause tap
            // made meanwhile (the watch looked unresponsive) would immediately pause it again.
            static int64_t started_us = 0;
            bool starting = m.cmd == static_cast<uint8_t>(swp::PlaybackCmd::PlayPlaylist) || m.cmd == static_cast<uint8_t>(swp::PlaybackCmd::Resume);
            if (m.cmd == static_cast<uint8_t>(swp::PlaybackCmd::Toggle) && m.queued_us < started_us) {
                ESP_LOGI(TAG, "ignoring play/pause tapped while starting");
                continue;
            }
            handle({m.cmd, m.arg, std::string(m.str)});
            if (starting) started_us = start_grace_us = esp_timer_get_time();
            continue;
        }
        if (busy) {
            if (!decode_step()) {
                // Let the ring drain before switching so the track tail is heard.
                while (ring_used() > 0 && playing.load()) vTaskDelay(pdMS_TO_TICKS(20));
                if (!advance(false)) {
                    playing.store(false);
                    close_track();
                    a2dp::suspend_stream();
                    set_state(swp::PlayState::Stopped);
                } else {
                    publish();
                }
            }
        }
        int64_t now = esp_timer_get_time();
        if (playing.load() && now - last_publish_us > 5000000) publish();
        // Release the media channel after an alert tone if nothing else plays.
        if (!playing.load() && alert_left.load() == 0 && a2dp::streaming() && now - last_publish_us > 1000000) a2dp::suspend_stream();
    }
}

void remote_cmd(swp::PlaybackCmd c) {
    if (memos::take_button()) return;  // stops a memo recording instead
    // A single press sends PLAY or PAUSE depending on what the headphones think is happening,
    // which can disagree with us (e.g. paused from the watch): treat both as a toggle.
    if (c == swp::PlaybackCmd::Resume || c == swp::PlaybackCmd::Pause) c = playing.load() ? swp::PlaybackCmd::Pause : swp::PlaybackCmd::Resume;
    command({static_cast<uint8_t>(c), 0, ""});
}

}  // namespace

void init() {
    ring = static_cast<int16_t*>(heap_caps_calloc(kRingFrames * 2, sizeof(int16_t), MALLOC_CAP_SPIRAM));
    inbuf = static_cast<uint8_t*>(heap_caps_malloc(kInBuf, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    outbuf_cap = 16 * 1024;
    outbuf = static_cast<uint8_t*>(heap_caps_malloc(outbuf_cap, MALLOC_CAP_SPIRAM));
    convbuf = static_cast<int16_t*>(heap_caps_malloc(kConvFrames * 4, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    file_buf = static_cast<char*>(heap_caps_malloc(32 * 1024, MALLOC_CAP_SPIRAM));
    if (!ring || !inbuf || !outbuf || !convbuf || !file_buf) {
        ESP_LOGE(TAG, "audio buffers allocation failed; playback disabled");
        return;
    }
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    q = xQueueCreate(8, sizeof(Msg));
    a2dp::init(pcm_source);
    a2dp::set_remote_handler(remote_cmd);
    // Highest application priority, on core 1 away from the Wi-Fi/BT controller tasks.
    xTaskCreatePinnedToCore(task, "decoder", 6144, nullptr, 18, nullptr, 1);  // ~2.3 KB used decoding MP3
}

void command(const swp::PlaybackCommand& c) {
    if (!q) return;
    if (c.str.size() >= sizeof(Msg::str)) {
        ESP_LOGW(TAG, "playlist id too long");
        return;
    }
    Msg m{};
    m.cmd = c.cmd;
    m.arg = c.arg;
    m.queued_us = esp_timer_get_time();
    memcpy(m.str, c.str.data(), c.str.size());
    if (xQueueSend(q, &m, 0) != pdTRUE) ESP_LOGW(TAG, "command queue full");
}

swp::PlaybackStatus status() {
    std::lock_guard<std::mutex> g(mu);
    swp::PlaybackStatus s = st;
    s.position_ms = static_cast<uint32_t>(played_frames.load() * 1000 / kRate);
    s.sink_connected = a2dp::connected();
    s.volume = a2dp::volume();
    return s;
}

bool active() { return playing.load(); }
bool streaming() { return a2dp::streaming(); }

void alert_tone(uint8_t repeats) {
    if (!a2dp::connected()) return;
    alert_phase.store(0);
    alert_left.store(static_cast<uint32_t>(std::min<uint8_t>(repeats, 10)) * 26460);
    a2dp::start_stream();
}

}  // namespace music::player
