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

class TracksScreen : public Screen {
public:
    TracksScreen(std::string id, std::string name) : id_(std::move(id)), name_(std::move(name)) {}
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
            tracks = s->tracks;
            total = s->tracks_total;
            cur_id = s->playback.track_id;
        }
        if (tracks.size() == shown_ && cur_id == shown_cur_) return;
        shown_ = tracks.size();
        shown_cur_ = cur_id;
        clear(list_);
        // Shuffle-all header row.
        std::string pid = id_;
        list_row(list_, "Shuffle", nullptr, nullptr, [pid] {
            swlink::playback(PlaybackCmd::SetShuffle, 1);
            swlink::playback(PlaybackCmd::PlayPlaylist, 0, pid);
            push(make_now_playing());
        }, color::pink(), LV_SYMBOL_SHUFFLE);
        for (auto& t : tracks) {
            std::string sub = t.artist + "  " + core::fmt_duration(t.duration_s);
            if (!t.available) sub += "  (not downloaded)";
            uint16_t idx = t.index;
            lv_obj_t* r = list_row(list_, t.title.c_str(), sub.c_str(), nullptr, [pid, idx] {
                swlink::playback(PlaybackCmd::SetShuffle, 0);
                swlink::playback(PlaybackCmd::PlayPlaylist, idx, pid);
                push(make_now_playing());
            });
            if (t.id == cur_id) lv_obj_set_style_text_color(r, color::pink(), 0);
            if (!t.available) lv_obj_set_style_text_opa(r, LV_OPA_60, 0);
        }
        if (tracks.size() < total) section_header(list_, "Loading...");
        if (total == 0) section_header(list_, "No tracks synced yet");
    }
    std::string id_, name_;
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
    if (!loaded) {
        section_header(list_, "Loading playlists...");
        return;
    }
    section_header(list_, "Playlists");
    for (auto& p : pls) {
        std::string sub = std::to_string(p.track_count) + " songs  " + core::fmt_duration(p.duration_s);
        if (!p.synced) sub += "  syncing";
        std::string id = p.id, name = p.name;
        list_row(list_, p.name.c_str(), sub.c_str(), nullptr, [id, name] { push(new TracksScreen(id, name)); }, color::pink(), LV_SYMBOL_AUDIO);
    }
    if (pls.empty()) section_header(list_, "No playlist synced. Configure Navidrome on the companion.");
}

class NowPlaying : public Screen {
public:
    const char* title() const override { return "Now Playing"; }
    bool scrollable() const override { return false; }
    uint32_t tick_ms() const override { return 500; }
    void build(lv_obj_t* c) override {
        // Drag down anywhere to dismiss back to the playlist.
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(
            c,
            [](lv_event_t*) {
                if (lv_indev_get_gesture_dir(lv_indev_active()) == LV_DIR_BOTTOM) pop();
            },
            LV_EVENT_GESTURE, nullptr);

        // Placeholder album art: rounded gradient tile with a note glyph (no artwork assets).
        lv_obj_t* art = lv_obj_create(c);
        lv_obj_set_size(art, 150, 150);
        lv_obj_align(art, LV_ALIGN_TOP_MID, 0, 6);
        lv_obj_set_style_radius(art, 24, 0);
        lv_obj_set_style_border_width(art, 0, 0);
        lv_obj_set_style_bg_color(art, color::pink(), 0);
        lv_obj_set_style_bg_grad_color(art, color::purple(), 0);
        lv_obj_set_style_bg_grad_dir(art, LV_GRAD_DIR_VER, 0);
        lv_obj_remove_flag(art, (lv_obj_flag_t)(LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE));
        lv_obj_t* note = label(art, LV_SYMBOL_AUDIO, &font_inter_32);
        lv_obj_center(note);

        title_ = label(c, "", &font_inter_24);
        lv_obj_set_width(title_, 370);
        lv_label_set_long_mode(title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
        lv_obj_set_style_text_align(title_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(title_, LV_ALIGN_TOP_MID, 0, 166);
        artist_ = label(c, "", &font_inter_20, color::muted());
        lv_obj_set_width(artist_, 370);
        lv_label_set_long_mode(artist_, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(artist_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(artist_, LV_ALIGN_TOP_MID, 0, 198);

        bar_ = lv_bar_create(c);
        lv_obj_set_size(bar_, 350, 6);
        lv_obj_align(bar_, LV_ALIGN_TOP_MID, 0, 236);
        lv_obj_set_style_bg_color(bar_, color::card2(), 0);
        lv_obj_set_style_bg_color(bar_, color::text(), LV_PART_INDICATOR);
        lv_bar_set_range(bar_, 0, 1000);
        time_ = label(c, "0:00 / -0:00", &font_inter_16, color::muted());
        lv_obj_align(time_, LV_ALIGN_TOP_MID, 0, 250);

        shuffle_ = round_button(c, LV_SYMBOL_SHUFFLE, color::card(), 56, [this] {
            swlink::playback(PlaybackCmd::SetShuffle, shuffle_on_ ? 0 : 1);
        });
        lv_obj_align(shuffle_, LV_ALIGN_TOP_LEFT, 18, 298);
        lv_obj_t* prev = round_button(c, LV_SYMBOL_PREV, color::card(), 72, [] { swlink::playback(PlaybackCmd::Previous); });
        lv_obj_align(prev, LV_ALIGN_TOP_LEFT, 84, 290);
        play_ = round_button(c, LV_SYMBOL_PLAY, color::text(), 88, [] { swlink::playback(PlaybackCmd::Toggle); });
        lv_obj_set_style_text_color(play_, color::bg(), 0);
        lv_obj_align(play_, LV_ALIGN_TOP_MID, 0, 282);
        lv_obj_t* next = round_button(c, LV_SYMBOL_NEXT, color::card(), 72, [] { swlink::playback(PlaybackCmd::Next); });
        lv_obj_align(next, LV_ALIGN_TOP_RIGHT, -84, 290);
        repeat_ = round_button(c, LV_SYMBOL_LOOP, color::card(), 56, [this] { swlink::playback(PlaybackCmd::SetRepeat, (repeat_mode_ + 1) % 3); });
        lv_obj_align(repeat_, LV_ALIGN_TOP_RIGHT, -18, 298);
        repeat_one_ = label(repeat_, "1", &font_inter_16, color::bg());
        lv_obj_align(repeat_one_, LV_ALIGN_TOP_RIGHT, -4, 2);

        err_ = label(c, "", &font_inter_16, color::orange());
        lv_obj_set_width(err_, 260);
        lv_obj_set_style_text_align(err_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(err_, LV_ALIGN_BOTTOM_MID, 0, -18);
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
        if (e.empty() && !p.sink_connected) e = "Headphones not connected (Settings > Connections > AirPods)";
        set_text(err_, e);
        update_time(p, rx);
    }
    void tick() override {
        app::Locked s;
        update_time(s->playback, s->playback_rx_us);
    }

private:
    void update_time(const swp::PlaybackStatus& p, int64_t rx) {
        uint32_t pos = live_position(p, rx);
        set_text(time_, core::fmt_elapsed_remaining(pos, p.duration_ms));
        lv_bar_set_value(bar_, p.duration_ms ? static_cast<int32_t>(1000ull * pos / p.duration_ms) : 0, LV_ANIM_OFF);
    }
    lv_obj_t *title_ = nullptr, *artist_ = nullptr, *bar_ = nullptr, *time_ = nullptr, *play_ = nullptr;
    lv_obj_t *shuffle_ = nullptr, *repeat_ = nullptr, *repeat_one_ = nullptr, *err_ = nullptr;
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
    swp::PlaybackStatus p;
    int64_t rx;
    {
        app::Locked s;
        p = s->playback;
        rx = s->playback_rx_us;
    }
    if (!has_track(p)) {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_HIDDEN);
    set_text(lv_obj_get_child(bar, 0), p.title);
    uint32_t pos = live_position(p, rx);
    lv_bar_set_value(lv_obj_get_child(bar, 1), p.duration_ms ? static_cast<int32_t>(1000ull * pos / p.duration_ms) : 0, LV_ANIM_OFF);
    bool playing = p.state == static_cast<uint8_t>(swp::PlayState::Playing);
    set_text(lv_obj_get_child(lv_obj_get_child(bar, 2), 0), playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

Screen* make_music() { return new MusicScreen(); }
Screen* make_now_playing() { return new NowPlaying(); }

}  // namespace ui
