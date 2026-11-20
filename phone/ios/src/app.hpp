// The iOS companion: speaks SWP to the watch over BLE and does what the ESP32 companion does
// (see firmware/companion). Everything runs on the main queue unless a comment says otherwise.
#pragma once

#import <Foundation/Foundation.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "swp/messages.hpp"

inline NSString* ns(const std::string& s) {
    return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding] ?: @"";
}
inline std::string str(NSString* s) { return s.UTF8String ? std::string(s.UTF8String) : std::string(); }

// Typed access to untrusted JSON: a wrong type reads as empty.
inline id jval(id o, NSString* k) { return [o isKindOfClass:NSDictionary.class] ? ((NSDictionary*)o)[k] : nil; }
inline double jnum(id o, NSString* k) {
    id v = jval(o, k);
    return [v isKindOfClass:NSNumber.class] ? [v doubleValue] : 0;
}
inline std::string jstr(id o, NSString* k) {
    id v = jval(o, k);
    if ([v isKindOfClass:NSString.class]) return str(v);
    if ([v isKindOfClass:NSNumber.class]) return std::to_string([v longLongValue]);
    return "";
}
inline NSArray* jarr(id o, NSString* k) {
    id v = jval(o, k);
    return [v isKindOfClass:NSArray.class] ? v : @[];
}
inline id jparse(const std::vector<uint8_t>& b) {
    if (b.empty()) return nil;
    return [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytesNoCopy:const_cast<uint8_t*>(b.data()) length:b.size() freeWhenDone:NO] options:0 error:nil];
}
inline double mono_s() { return NSProcessInfo.processInfo.systemUptime; }

// A short log that survives relaunches, shown on the status screen.
namespace diag {
void note(const std::string& s);  // main queue
std::string text();
}  // namespace diag

namespace cfg {
constexpr const char* kServerUrl = "srv_url";
constexpr const char* kServerToken = "srv_token";
constexpr const char* kNdUrl = "nd_url";
constexpr const char* kNdUser = "nd_user";
constexpr const char* kNdPass = "nd_pass";
constexpr const char* kNdToken = "nd_token";
constexpr const char* kNtfyUrl = "ntfy_url";
constexpr const char* kNtfyToken = "ntfy_token";
constexpr const char* kNtfyTopics = "ntfy_topics";
constexpr const char* kTimezone = "tz";
constexpr const char* kDeviceName = "dev_name";

struct KeyInfo {
    const char* key;
    bool secret;
    size_t max_len;
    const char* label;
};
const std::vector<KeyInfo>& keys();
const KeyInfo* find(const std::string& key);
std::string get(const char* key, const std::string& def = "");  // any thread
bool is_set(const char* key);
bool set(const std::string& key, const std::string& value);
void add_listener(std::function<void(const std::string&)> l);
}  // namespace cfg

namespace paths {
std::string music();
std::string memos();
std::string maps();
uint64_t free_bytes();
uint64_t total_bytes();
}  // namespace paths

namespace net {
void start();
bool online();
bool wifi();
void add_listener(std::function<void()> l);
void mark_internet();
uint32_t internet_age_s();
void set_server_ok(bool ok);
bool server_ok();
swp::WifiStatus status();
}  // namespace net

namespace http {
using Headers = std::vector<std::pair<std::string, std::string>>;
struct Result {
    int status = 0;  // 0: transport error
    std::string error;
    std::string mime;
    std::vector<uint8_t> body;
    bool ok() const { return status >= 200 && status < 300; }
};
struct Options {
    std::string method = "GET";
    Headers headers;
    std::vector<uint8_t> body;
    size_t max_response = 64 * 1024;
    double timeout_s = 30;
};
// Certificates are always verified by the system; redirects may not downgrade from https or
// leave a plain-http origin. `done` runs on the main queue.
void request(const std::string& url, Options o, std::function<void(Result)> done);
// Downloads to `dest` (replaced on success), up to o.max_response bytes. `body` stays empty.
void download(const std::string& url, const std::string& dest, Options o, std::function<void(Result)> done);
// Blocking forms for background queues (never call from the main queue).
Result request_sync(const std::string& url, Options o);
Result download_sync(const std::string& url, const std::string& dest, Options o);
}  // namespace http

namespace ble {
void start();
void forget();    // drops the remembered watch and stops looking
void connect();   // looks for a watch again after forget()
bool up();        // notifications subscribed: frames can be sent
bool has_peer();
std::string status_text();
std::string last_drop_text();  // why the link last went down
std::string debug_text();      // what the current or last link carried
bool send_frame(uint8_t type, const std::vector<uint8_t>& payload);  // any thread
template <class M>
bool send(const M& m) { return send_frame(static_cast<uint8_t>(M::kType), swp::encode_payload(m)); }
size_t queued();
uint16_t chunk_size();  // TransferChunk payload that fits one BLE write
}  // namespace ble

namespace dispatch {
void start();
void on_frame(const swp::Frame& f);
void on_link_up();
void on_link_down();
bool watch_ready();
void broadcast_status();
}  // namespace dispatch

namespace position {
void start();
bool current(swp::Position& out);
std::string status_text();
}  // namespace position

namespace cloud {
void start();
void request_tile(const swp::MapTileRequest& r);
void request_search(const swp::MapSearchRequest& r);
void request_route(const swp::RouteRequest& r);
void request_weather();
void request_map_data(const swp::MapDataCommand& c);
void request_http(swp::HttpRequest r);
void on_transfer_ack(const swp::TransferAck& a);
void on_watch_connected();
void on_watch_disconnected();
bool weather(swp::Weather& out);
void ingest_notification(const swp::Notification& n);
void on_notification_ack(uint64_t id);
uint16_t pending_notifications();
void pump();  // the BLE queue has room
uint64_t maps_used();
}  // namespace cloud

namespace ntfy {
void start();
void on_ack(uint64_t key);
uint32_t last_event_utc();
bool connected();
}  // namespace ntfy

namespace library {
constexpr const char* kAllSongsId = "all";
constexpr const char* kSearchPrefix = "search:";
struct Track {
    std::string id, title, artist, album, suffix;
    uint32_t duration_s = 0;
    uint64_t size = 0;
    bool available = false;
    std::string path;  // when not in the music directory (voice memos)
};
struct Playlist {
    std::string id, name;
    std::vector<uint32_t> tracks;
    uint32_t duration_s = 0;
};
// Thread-safe: navidrome runs on a background queue.
void load();
bool save();
void replace(std::vector<Track> tracks, std::vector<Playlist> lists);
void set_available(const std::string& id, uint64_t size);
std::vector<swp::PlaylistInfo> playlists();
swp::PlaylistTracks page(const std::string& id, uint16_t offset, uint16_t count);
std::vector<Track> tracks_of(const std::string& id);
size_t downloaded();
std::string path_for(const std::string& id, const std::string& suffix);
}  // namespace library

namespace navidrome {
void start();
void trigger();
swp::SyncState state();
std::string last_error();  // empty unless the current sync hit an error
uint16_t done();
uint16_t total();
uint64_t used_bytes();
}  // namespace navidrome

@class UIView;

namespace player {
void start();
void command(const swp::PlaybackCommand& c);
swp::PlaybackStatus status();
bool active();
void pause_for_memo();
void apply_session();  // audio session for what is going on now
void attach_volume_view(UIView* parent);  // lets the player raise the system volume
void set_keep_alive(bool on);
bool keep_alive();
void push_audio_status();
swp::AudioStatus audio_status();
swp::AudioDevices audio_devices();
}  // namespace player

namespace memos {
constexpr const char* kPlaylistId = "memos";
void command(const swp::MemoCommand& c);
swp::MemoStatus status();
swp::MemoList list();
std::vector<library::Track> tracks();  // same order as list()
bool active();
bool take_button();  // a headphone button press stops a recording instead of playing music
}  // namespace memos

namespace ui {
id make_root();
}
