// Music: playlists -> tracks -> compact mini player -> full "now playing" player.
#include "app/state.hpp"
#include "ble/link.hpp"
#include "core/format.hpp"
#include "esp_timer.h"
#include "ui/ui.hpp"

namespace ui {
namespace {

using swp::PlaybackCmd;

// Interpolates the playback position between status updates (sent every ~5 s).
uint32_t live_position(const swp::PlaybackStatus& p, int64_t rx_us) {
    uint32_t pos = p.position_ms;
    if (p.state == static_cast<uint8_t>(swp::PlayState::Playing)) pos += static_cast<uint32_t>((esp_timer_get_time() - rx_us) / 1000);
    if (p.duration_ms && pos > p.duration_ms) pos = p.duration_ms;
    return pos;
}

bool has_track(const swp::PlaybackStatus& p) {
    auto s = static_cast<swp::PlayState>(p.state);
    return !p.track_id.empty() && (s == swp::PlayState::Playing || s == swp::PlayState::Paused || s == swp::PlayState::Buffering);
}

class TracksScreen;

class MusicScreen : public Screen {
public:
    const char* title() const override { return "Music"; }
    bool scrollable() const override { return false; }
    void build(lv_obj_t* c) override {
        list_ = lv_obj_create(c);
        lv_obj_remove_style_all(list_);
        lv_obj_set_size(list_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(list_, 12, 0);
        lv_obj_set_style_pad_row(list_, 10, 0);
        lv_obj_set_style_pad_bottom(list_, 110, 0);
        mini_ = mini_player(c);
        swlink::request_playlists();
        refresh(0xFFFFFFFFu);
    }
    void refresh(uint32_t d) override {
        if (d & (app::kDirtyPlaylists | app::kDirtyLink)) rebuild();
        if (d & app::kDirtyPlayback) mini_player_refresh(mini_);
    }
    void tick() override { mini_player_refresh(mini_); }

private:
    void rebuild();
    lv_obj_t* list_ = nullptr;
    lv_obj_t* mini_ = nullptr;
};

// Virtual playlists on the companion (see music/library.hpp there).
constexpr const char* kAllSongs = "all";
constexpr const char* kSearchPrefix = "search:";

// Shuffle everything downloaded, from a random track, repeating all.
void shuffle_all() {
    swlink::playback(PlaybackCmd::SetShuffle, 1);
    swlink::playback(PlaybackCmd::SetRepeat, static_cast<uint32_t>(swp::RepeatMode::All));
    swlink::playback(PlaybackCmd::PlayPlaylist, UINT32_MAX, kAllSongs);
}

class TracksScreen : public Screen {
public:
    // A playlist, or song search results (`search`: picking a song plays it on repeat).
    TracksScreen(std::string id, std::string name, bool search = false) : id_(std::move(id)), name_(std::move(name)), search_(search) {}
    const char* title() const override { return name_.c_str(); }
    bool scrollable() const override { return false; }
    void build(lv_obj_t* c) override {
        list_ = lv_obj_create(c);
        lv_obj_remove_style_all(list_);
        lv_obj_set_size(list_, LV_PCT(100), LV_PCT(100));
        lv_obj_set_flex_flow(list_, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_all(list_, 12, 0);
        lv_obj_set_style_pad_row(list_, 8, 0);
        lv_obj_set_style_pad_bottom(list_, 110, 0);
        lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);
        mini_ = mini_player(c);
        swlink::request_tracks(id_, 0);
        refresh(0xFFFFFFFFu);
    }
    void refresh(uint32_t d) override {
        if (d & app::kDirtyTracks) rebuild();
        if (d & app::kDirtyPlayback) {
            mini_player_refresh(mini_);
            rebuild();  // highlight the current track
        }
    }
    void tick() override { mini_player_refresh(mini_); }

private:
    void rebuild() {
        std::vector<swp::TrackInfo> tracks;
        uint16_t total = 0;
        std::string cur_id;
        {
            app::Locked s;
            if (s->tracks_playlist != id_) return;
            if (s->tracks.size() == shown_ && s->playback.track_id == shown_cur_) return;
            tracks = s->tracks;
            total = s->tracks_total;
            cur_id = s->playback.track_id;
        }
        shown_ = tracks.size();
        shown_cur_ = cur_id;
        clear(list_);
        std::string pid = id_;
        bool search = search_;
        if (!search && total > 0) {
            // Shuffle-this-playlist header row.
            list_row(list_, "Shuffle", nullptr, nullptr, [pid] {
                swlink::playback(PlaybackCmd::SetShuffle, 1);
                swlink::playback(PlaybackCmd::PlayPlaylist, UINT32_MAX, pid);
                push(make_now_playing());
            }, color::pink(), LV_SYMBOL_SHUFFLE);
        }
        for (auto& t : tracks) {
            std::string sub = t.artist + " \xC2\xB7 " + core::fmt_duration(t.duration_s);
            if (!t.available) sub += "  (not downloaded)";
            uint16_t idx = t.index;
            lv_obj_t* r = list_row(list_, t.title.c_str(), sub.c_str(), nullptr, [pid, idx, search] {
                swlink::playback(PlaybackCmd::SetShuffle, 0);
                if (search) swlink::playback(PlaybackCmd::SetRepeat, static_cast<uint32_t>(swp::RepeatMode::One));
                swlink::playback(PlaybackCmd::PlayPlaylist, idx, pid);
                push(make_now_playing());
            });
            if (t.id == cur_id) lv_obj_set_style_text_color(r, color::pink(), 0);
            if (!t.available) lv_obj_set_style_text_opa(r, LV_OPA_60, 0);
        }
        if (tracks.size() >= swlink::kMaxListedTracks && tracks.size() < total)
            section_header(list_, ("First " + std::to_string(tracks.size()) + " of " + std::to_string(total) + " songs. Shuffle plays them all.").c_str());
        else if (tracks.size() < total)
            section_header(list_, "Loading...");
        if (total == 0) section_header(list_, search ? "No matching downloaded songs" : "No tracks synced yet");
    }
    std::string id_, name_;
    bool search_ = false;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* mini_ = nullptr;
    size_t shown_ = SIZE_MAX;
    std::string shown_cur_;
};

void MusicScreen::rebuild() {
    clear(list_);
    std::vector<swp::PlaylistInfo> pls;
    bool loaded, ready;
    {
        app::Locked s;
        pls = s->playlists;
        loaded = s->playlists_loaded;
        ready = s->link.state == app::LinkState::Ready;
    }
    if (!ready) {
        section_header(list_, "Companion not connected");
        list_row(list_, "Connections", "Pair or reconnect the companion", nullptr, [] { push(make_settings()); }, color::blue(), LV_SYMBOL_BLUETOOTH);
        return;
    }
    // Search | Shuffle all, equal halves.
    lv_obj_t* top = lv_obj_create(list_);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(top, 8, 0);
    lv_obj_t* search = list_row(top, "Search", nullptr, nullptr, [] {
        KeyboardOpts o;
        o.label = "Song or artist";
        o.max_len = 40;  // the id ("search:" + text) must fit the companion's 64-byte command
        open_keyboard(o, [](const std::string& q) {
            if (q.empty()) return;
            push(new TracksScreen(kSearchPrefix + q, q, true));
        });
    }, color::blue(), UI_SYMBOL_SEARCH);
    lv_obj_t* shuf = list_row(top, "Shuffle all", nullptr, nullptr, [] {
        shuffle_all();
        push(make_now_playing());
    }, color::pink(), LV_SYMBOL_SHUFFLE);
    for (lv_obj_t* b : {search, shuf}) {
        lv_obj_set_width(b, 0);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_style_pad_hor(b, 10, 0);
        lv_obj_set_style_pad_column(b, 8, 0);
    }
    if (!loaded) {
        section_header(list_, "Loading playlists...");
        return;
    }
    section_header(list_, "Playlists");
    for (auto& p : pls) {
        if (p.id == kAllSongs) continue;
        std::string sub = std::to_string(p.track_count) + " songs \xC2\xB7 " + core::fmt_duration(p.duration_s);
        if (!p.synced) sub += "  syncing";
        std::string id = p.id, name = p.name;
        list_row(list_, p.name.c_str(), sub.c_str(), nullptr, [id, name] { push(new TracksScreen(id, name)); }, color::pink(), LV_SYMBOL_AUDIO);
    }
    if (pls.empty()) section_header(list_, "No music synced. Configure Navidrome on the companion.");
}

class NowPlaying : public Screen {
public:
    const char* title() const override { return "Now Playing"; }
    uint32_t tick_ms() const override { return 100; }  // elapsed/remaining tick independently
    void build(lv_obj_t* c) override {
        // Drag down (at the top) to dismiss back to the playlist; no elastic overscroll so the
        // drag reaches the gesture handler.
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLL_ELASTIC);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(
            c,
            [](lv_event_t* e) {
                lv_obj_t* t = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
                if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM && lv_obj_get_scroll_y(t) <= 0) pop();
            },
            LV_EVENT_GESTURE, nullptr);

        // Placeholder album art: rounded gradient tile with a note glyph (no artwork assets).
        lv_obj_t* art = lv_obj_create(c);
        lv_obj_set_size(art, 104, 104);
        lv_obj_set_pos(art, kSide, 0);
        lv_obj_set_style_radius(art, 22, 0);
        lv_obj_set_style_border_width(art, 0, 0);
        lv_obj_set_style_bg_color(art, color::pink(), 0);
        lv_obj_set_style_bg_grad_color(art, color::purple(), 0);
        lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
        lv_obj_remove_flag(art, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_center(label(art, LV_SYMBOL_AUDIO, &font_inter_32));

        title_ = label(c, "", &font_inter_24);
        lv_obj_set_width(title_, kW - 120);
        lv_label_set_long_mode(title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_pos(title_, kSide + 120, 22);
        artist_ = label(c, "", &font_inter_20, color::muted());
        lv_obj_set_width(artist_, kW - 120);
        lv_label_set_long_mode(artist_, LV_LABEL_LONG_DOT);
        lv_obj_set_pos(artist_, kSide + 120, 58);

        // Progress: drag to scrub, seeks on release.
        bar_ = slider(c, kW, 0, 1000);
        lv_obj_set_pos(bar_, kSide, 126);
        on(bar_, LV_EVENT_PRESSED, [this] { scrubbing_ = true; });
        on(bar_, LV_EVENT_VALUE_CHANGED, [this] {
            if (!scrubbing_) return;
            uint32_t dur;
            {
                app::Locked s;
                dur = s->playback.duration_ms;
            }
            show_times(static_cast<uint32_t>(1ull * lv_slider_get_value(bar_) * dur / 1000), dur);
        });
        auto seek = [this] {
            if (!scrubbing_) return;
            scrubbing_ = false;
            uint32_t dur;
            {
                app::Locked s;
                dur = s->playback.duration_ms;
            }
            if (!dur) return;
            // Hold the released position until the companion reports it (otherwise the knob
            // snaps back to the old position for a moment).
            seek_ms_ = static_cast<uint32_t>(1ull * lv_slider_get_value(bar_) * dur / 1000);
            seek_at_us_ = esp_timer_get_time();
            swlink::playback(PlaybackCmd::Seek, seek_ms_);
        };
        on(bar_, LV_EVENT_RELEASED, seek);
        on(bar_, LV_EVENT_PRESS_LOST, seek);
        elapsed_ = label(c, "0:00", &font_inter_16, color::muted());
        lv_obj_set_pos(elapsed_, kSide, 142);
        remaining_ = label(c, "-0:00", &font_inter_16, color::muted());
        lv_obj_align(remaining_, LV_ALIGN_TOP_RIGHT, -kSide, 142);

        shuffle_ = round_button(c, LV_SYMBOL_SHUFFLE, color::card(), 56, [this] {
            swlink::playback(PlaybackCmd::SetShuffle, shuffle_on_ ? 0 : 1);
        });
        lv_obj_align(shuffle_, LV_ALIGN_TOP_LEFT, 18, 186);
        lv_obj_t* prev = round_button(c, LV_SYMBOL_PREV, color::card(), 72, [] { swlink::playback(PlaybackCmd::Previous); });
        lv_obj_align(prev, LV_ALIGN_TOP_LEFT, 84, 178);
        play_ = round_button(c, LV_SYMBOL_PLAY, color::text(), 88, [] { swlink::playback(PlaybackCmd::Toggle); });
        lv_obj_set_style_text_color(play_, color::bg(), 0);
        lv_obj_align(play_, LV_ALIGN_TOP_MID, 0, 170);
        lv_obj_t* next = round_button(c, LV_SYMBOL_NEXT, color::card(), 72, [] { swlink::playback(PlaybackCmd::Next); });
        lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -84, 178);
        repeat_ = round_button(c, LV_SYMBOL_LOOP, color::card(), 56, [this] { swlink::playback(PlaybackCmd::SetRepeat, (repeat_mode_ + 1) % 3); });
        lv_obj_align(repeat_, LV_ALIGN_TOP_RIGHT, -18, 186);
        repeat_one_ = label(repeat_, "1", &font_inter_16, color::text());
        lv_obj_align(repeat_one_, LV_ALIGN_TOP_RIGHT, -11, 9);

        // Volume (0..127, AVRCP absolute volume or software gain on the companion).
        lv_obj_t* vic = label(c, LV_SYMBOL_VOLUME_MAX, &font_inter_20, color::muted());
        lv_obj_set_pos(vic, kSide, 282);
        vol_ = slider(c, kW - 42, 0, 127);
        lv_obj_set_pos(vol_, kSide + 42, 290);
        on(vol_, LV_EVENT_PRESSED, [this] { vol_dragging_ = true; });
        auto set_vol = [this] {
            if (!vol_dragging_) return;
            vol_dragging_ = false;
            vol_pending_ = lv_slider_get_value(vol_);
            vol_at_us_ = esp_timer_get_time();
            swlink::playback(PlaybackCmd::SetVolume, static_cast<uint32_t>(vol_pending_));
        };
        on(vol_, LV_EVENT_RELEASED, set_vol);
        on(vol_, LV_EVENT_PRESS_LOST, set_vol);

        err_ = label(c, "", &font_inter_16, color::orange());
        lv_obj_set_width(err_, kW);
        lv_obj_set_style_text_align(err_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(err_, kSide, 318);

        // Up next: 2x2 grid, tap to skip there.
        up_hdr_ = label(c, "Up next", &font_inter_20);
        lv_obj_set_pos(up_hdr_, kSide + 4, 346);
        constexpr int kCellW = (kW - 8) / 2, kCellH = 68;
        for (int k = 0; k < 4; ++k) {
            lv_obj_t* cell = card(c);
            lv_obj_set_size(cell, kCellW, kCellH);
            lv_obj_set_pos(cell, kSide + (k % 2) * (kCellW + 8), 378 + (k / 2) * (kCellH + 8));
            lv_obj_set_style_radius(cell, 18, 0);
            lv_obj_set_style_pad_hor(cell, 12, 0);
            lv_obj_set_style_pad_ver(cell, 8, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            uint32_t pos = static_cast<uint32_t>(k + 1);
            on_click(cell, [pos] { swlink::playback(PlaybackCmd::SkipTo, pos); });
            lv_obj_t* t = label(cell, "", &font_inter_20);
            lv_obj_set_width(t, LV_PCT(100));
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
            lv_obj_t* a = label(cell, "", &font_inter_16, color::muted());
            lv_obj_set_width(a, LV_PCT(100));
            lv_label_set_long_mode(a, LV_LABEL_LONG_DOT);
            lv_obj_align(a, LV_ALIGN_BOTTOM_LEFT, 0, 0);
            cells_[k] = cell;
        }
        refresh(0xFFFFFFFFu);
    }
    void refresh(uint32_t d) override {
        if (!(d & (app::kDirtyPlayback | app::kDirtyAudio))) return;
        swp::PlaybackStatus p;
        int64_t rx;
        {
            app::Locked s;
            p = s->playback;
            rx = s->playback_rx_us;
        }
        set_text(title_, p.title.empty() ? "Not playing" : p.title);
        set_text(artist_, p.artist);
        bool playing = p.state == static_cast<uint8_t>(swp::PlayState::Playing);
        lv_obj_t* pl = lv_obj_get_child(play_, 0);
        set_text(pl, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_obj_set_style_text_color(pl, color::bg(), 0);
        shuffle_on_ = p.shuffle;
        repeat_mode_ = p.repeat;
        lv_obj_set_style_bg_color(shuffle_, p.shuffle ? color::pink() : color::card(), 0);
        lv_obj_set_style_bg_color(repeat_, p.repeat ? color::pink() : color::card(), 0);
        if (p.repeat == static_cast<uint8_t>(swp::RepeatMode::One)) lv_obj_remove_flag(repeat_one_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(repeat_one_, LV_OBJ_FLAG_HIDDEN);
        std::string e = p.error;
        if (e.empty() && !p.sink_connected && !playing) e = "Headphones connect when you press play";
        set_text(err_, e);
        for (size_t k = 0; k < 4; ++k) {
            if (k < p.up_next.size()) {
                lv_obj_remove_flag(cells_[k], LV_OBJ_FLAG_HIDDEN);
                set_text(lv_obj_get_child(cells_[k], 0), p.up_next[k].title);
                set_text(lv_obj_get_child(cells_[k], 1), p.up_next[k].artist);
            } else {
                lv_obj_add_flag(cells_[k], LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (p.up_next.empty()) lv_obj_add_flag(up_hdr_, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_remove_flag(up_hdr_, LV_OBJ_FLAG_HIDDEN);
        update_time(p, rx);
    }
    void tick() override {
        app::Locked s;
        update_time(s->playback, s->playback_rx_us);
    }

private:
    static constexpr int kSide = 36;            // side margin
    static constexpr int kW = 410 - 2 * kSide;  // content width
    static constexpr int64_t kHoldUs = 3000000;  // max wait for the companion to confirm a slider change

    void show_times(uint32_t pos, uint32_t dur) {
        // Elapsed rounds down and remaining up, each from the exact millisecond position, so
        // the two tick at different moments (unless the duration is a whole second).
        if (dur && pos > dur) pos = dur;
        set_text(elapsed_, core::fmt_duration(pos / 1000));
        set_text(remaining_, "-" + core::fmt_duration(dur > pos ? (dur - pos + 999) / 1000 : 0));
    }
    void update_time(const swp::PlaybackStatus& p, int64_t rx) {
        int64_t now = esp_timer_get_time();
        if (vol_pending_ >= 0 && (p.volume == vol_pending_ || now - vol_at_us_ > kHoldUs)) vol_pending_ = -1;
        if (!vol_dragging_ && vol_pending_ < 0 && lv_slider_get_value(vol_) != p.volume) lv_slider_set_value(vol_, p.volume, LV_ANIM_OFF);
        if (scrubbing_) return;
        uint32_t pos = live_position(p, rx);
        if (seek_at_us_) {
            uint32_t want = seek_ms_;
            if (p.state == static_cast<uint8_t>(swp::PlayState::Playing)) want += static_cast<uint32_t>((now - seek_at_us_) / 1000);
            bool confirmed = rx > seek_at_us_ && (pos > want ? pos - want : want - pos) < 1500;
            if (confirmed || now - seek_at_us_ > kHoldUs) seek_at_us_ = 0;
            else pos = want;
        }
        show_times(pos, p.duration_ms);
        lv_slider_set_value(bar_, p.duration_ms ? static_cast<int32_t>(1000ull * pos / p.duration_ms) : 0, LV_ANIM_OFF);
    }
    static lv_obj_t* slider(lv_obj_t* parent, int w, int32_t lo, int32_t hi) {
        lv_obj_t* sl = lv_slider_create(parent);
        lv_obj_set_size(sl, w, 6);
        lv_slider_set_range(sl, lo, hi);
        lv_obj_set_style_bg_color(sl, lv_color_hex(0x5A5A5E), 0);
        lv_obj_set_style_bg_opa(sl, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(sl, color::text(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(sl, color::text(), LV_PART_KNOB);
        lv_obj_set_style_pad_all(sl, 7, LV_PART_KNOB);  // 20 px knob
        lv_obj_set_ext_click_area(sl, 18);              // easier to grab with a finger
        lv_obj_remove_flag(sl, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE));
        return sl;
    }
    lv_obj_t *title_ = nullptr, *artist_ = nullptr, *bar_ = nullptr, *elapsed_ = nullptr, *remaining_ = nullptr, *play_ = nullptr;
    lv_obj_t *shuffle_ = nullptr, *repeat_ = nullptr, *repeat_one_ = nullptr, *err_ = nullptr, *vol_ = nullptr, *up_hdr_ = nullptr;
    lv_obj_t* cells_[4] = {};
    bool scrubbing_ = false, vol_dragging_ = false;
    uint32_t seek_ms_ = 0;
    int64_t seek_at_us_ = 0, vol_at_us_ = 0;
    int32_t vol_pending_ = -1;
    bool shuffle_on_ = false;
    uint8_t repeat_mode_ = 0;
};

}  // namespace

lv_obj_t* mini_player(lv_obj_t* parent) {
    lv_obj_t* bar = card(parent);
    lv_obj_set_size(bar, 320, 92);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_bg_color(bar, color::card2(), 0);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    on_click(bar, [] { push(make_now_playing()); });
    lv_obj_t* t = label(bar, "", &font_inter_20);
    lv_obj_set_width(t, 160);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 4, 4);
    lv_obj_t* pb = lv_bar_create(bar);
    lv_obj_set_size(pb, 160, 5);
    lv_obj_align(pb, LV_ALIGN_BOTTOM_LEFT, 4, -10);
    lv_obj_set_style_bg_color(pb, color::card(), 0);
    lv_obj_set_style_bg_color(pb, color::pink(), LV_PART_INDICATOR);
    lv_bar_set_range(pb, 0, 1000);
    lv_obj_t* play = round_button(bar, LV_SYMBOL_PLAY, color::card(), 60, [] { swlink::playback(PlaybackCmd::Toggle); });
    lv_obj_align(play, LV_ALIGN_RIGHT_MID, -68, 0);
    lv_obj_t* next = round_button(bar, LV_SYMBOL_NEXT, color::card(), 60, [] { swlink::playback(PlaybackCmd::Next); });
    lv_obj_align(next, LV_ALIGN_RIGHT_MID, 0, 0);
    mini_player_refresh(bar);
    return bar;
}

void mini_player_refresh(lv_obj_t* bar) {
    if (!bar) return;
    app::Locked s;
    const auto& p = s->playback;
    if (!has_track(p)) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
    set_text(lv_obj_get_child(bar, 0), p.title);
    uint32_t pos = live_position(p, s->playback_rx_us);
    lv_bar_set_value(lv_obj_get_child(bar, 1), p.duration_ms ? static_cast<int32_t>(1000ull * pos / p.duration_ms) : 0, LV_ANIM_OFF);
    bool playing = p.state == static_cast<uint8_t>(swp::PlayState::Playing);
    set_text(lv_obj_get_child(lv_obj_get_child(bar, 2), 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

Screen* make_music() { return new MusicScreen(); }
Screen* make_now_playing() { return new NowPlaying(); }

}  // namespace ui
