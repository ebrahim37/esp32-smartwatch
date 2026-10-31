// SWP message catalogue. See docs/protocol.md for the full description.
//
// Compatibility rules:
//   * The frame header carries the protocol major version; peers with a different major
//     version reject each other's frames.
//   * Within a major version, fields may only be appended. Appended fields are guarded with
//     `if (!a.more()) return;` and decoders ignore unknown trailing bytes.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "swp/frame.hpp"
#include "swp/wire.hpp"

namespace swp {

// GATT layout shared by the companion and the watch's configuration service.
// Base UUID 7a1eXXXX-5c3b-4f8e-9d2a-6b1e5a7c0de0.
constexpr const char* kServiceUuid = "7a1e0001-5c3b-4f8e-9d2a-6b1e5a7c0de0";
constexpr const char* kRxCharUuid = "7a1e0002-5c3b-4f8e-9d2a-6b1e5a7c0de0";  // write / write-no-rsp
constexpr const char* kTxCharUuid = "7a1e0003-5c3b-4f8e-9d2a-6b1e5a7c0de0";  // notify
constexpr const char* kInfoCharUuid = "7a1e0004-5c3b-4f8e-9d2a-6b1e5a7c0de0"; // read, unauthenticated
// 128-bit UUIDs in little-endian byte order, as used by both BLE stacks.
constexpr uint8_t kServiceUuid128[16] = {0xe0, 0x0d, 0x7c, 0x5a, 0x1e, 0x6b, 0x2a, 0x9d,
                                         0x8e, 0x4f, 0x3b, 0x5c, 0x01, 0x00, 0x1e, 0x7a};
constexpr uint8_t kRxCharUuid128[16] = {0xe0, 0x0d, 0x7c, 0x5a, 0x1e, 0x6b, 0x2a, 0x9d,
                                        0x8e, 0x4f, 0x3b, 0x5c, 0x02, 0x00, 0x1e, 0x7a};
constexpr uint8_t kTxCharUuid128[16] = {0xe0, 0x0d, 0x7c, 0x5a, 0x1e, 0x6b, 0x2a, 0x9d,
                                        0x8e, 0x4f, 0x3b, 0x5c, 0x03, 0x00, 0x1e, 0x7a};
constexpr uint8_t kInfoCharUuid128[16] = {0xe0, 0x0d, 0x7c, 0x5a, 0x1e, 0x6b, 0x2a, 0x9d,
                                          0x8e, 0x4f, 0x3b, 0x5c, 0x04, 0x00, 0x1e, 0x7a};

enum class MsgType : uint8_t {
    Hello = 0x01,
    HelloAck = 0x02,
    Heartbeat = 0x03,
    Error = 0x04,

    TimeSync = 0x10,
    TimeRequest = 0x11,

    Position = 0x20,
    WeatherRequest = 0x28,
    Weather = 0x29,

    MapTileRequest = 0x30,
    MapSearchRequest = 0x31,
    MapSearchResults = 0x32,
    RouteRequest = 0x33,
    Route = 0x34,
    MapSyncRequest = 0x35,
    MapSyncStatus = 0x36,
    MapDataCommand = 0x3C,
    MapDataStatus = 0x3D,

    TransferBegin = 0x38,
    TransferChunk = 0x39,
    TransferAck = 0x3A,
    TransferAbort = 0x3B,

    Notification = 0x40,
    NotificationAck = 0x41,

    PlaylistsRequest = 0x50,
    Playlists = 0x51,
    PlaylistRequest = 0x52,
    PlaylistTracks = 0x53,
    PlaybackCommand = 0x54,
    PlaybackStatus = 0x55,

    WifiConfigSet = 0x60,
    WifiStatusRequest = 0x61,
    WifiStatus = 0x62,

    AudioScanStart = 0x68,
    AudioDevices = 0x69,
    AudioConnect = 0x6A,
    AudioDisconnect = 0x6B,
    AudioForget = 0x6C,
    AudioStatus = 0x6D,

    ConfigSet = 0x70,
    ConfigGet = 0x71,
    ConfigValue = 0x72,
    WebConfigCommand = 0x73,
    WebConfigStatus = 0x74,

    StatusRequest = 0x78,
    CompanionStatus = 0x79,
    DeviceInfo = 0x7A,
    WatchSettings = 0x7B,
};

const char* msg_type_name(MsgType t);

enum Capability : uint32_t {
    kCapTime = 1u << 0,
    kCapPosition = 1u << 1,
    kCapWeather = 1u << 2,
    kCapMaps = 1u << 3,
    kCapNotifications = 1u << 4,
    kCapMusic = 1u << 5,
    kCapWifiConfig = 1u << 6,
    kCapAudioDevices = 1u << 7,
    kCapConfig = 1u << 8,
    kCapTransfer = 1u << 9,
};

enum class Role : uint8_t { Watch = 1, Companion = 2, Phone = 4 };

// Shorthand for declaring a message struct's type tag.
#define SWP_MSG(name) static constexpr MsgType kType = MsgType::name

struct Hello {
    SWP_MSG(Hello);
    uint8_t proto_version = kProtocolVersion;
    uint8_t min_proto_version = kProtocolVersion;
    uint8_t role = 0;
    uint32_t capabilities = 0;
    std::string firmware;
    std::string device_name;
    template <class A>
    void fields(A& a) {
        a.u8(proto_version); a.u8(min_proto_version); a.u8(role); a.u32(capabilities);
        a.str8(firmware); a.str8(device_name);
    }
};

struct HelloAck : Hello {
    SWP_MSG(HelloAck);
};

struct Heartbeat {
    SWP_MSG(Heartbeat);
    uint32_t uptime_s = 0;
    uint8_t flags = 0;  // bit0: audio playing (peer should avoid bulk transfers)
    template <class A>
    void fields(A& a) { a.u32(uptime_s); a.u8(flags); }
};
constexpr uint8_t kHeartbeatAudioActive = 1u << 0;

enum class ErrorCode : uint8_t {
    Unknown = 0, BadRequest = 1, NotSupported = 2, Unavailable = 3, NoNetwork = 4,
    NotFound = 5, Busy = 6, Unauthorized = 7, Storage = 8, Timeout = 9,
};

struct Error {
    SWP_MSG(Error);
    uint8_t code = 0;
    uint8_t ref_type = 0;
    std::string message;
    template <class A>
    void fields(A& a) { a.u8(code); a.u8(ref_type); a.str8(message); }
};

enum class TimeSource : uint8_t { None = 0, Ntp = 1, Gnss = 2, Cellular = 3, Phone = 4, Manual = 5 };

struct TimeSync {
    SWP_MSG(TimeSync);
    uint64_t utc_ms = 0;
    uint8_t source = 0;
    std::string posix_tz;  // e.g. "EST5EDT,M3.2.0,M11.1.0"; empty keeps the watch setting
    template <class A>
    void fields(A& a) { a.u64(utc_ms); a.u8(source); a.str8(posix_tz); }
};

struct TimeRequest {
    SWP_MSG(TimeRequest);
    template <class A>
    void fields(A&) {}
};

enum class PositionSource : uint8_t { None = 0, Simulated = 1, Gnss = 2, Phone = 3, Network = 4 };

struct Position {
    SWP_MSG(Position);
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
    uint16_t accuracy_m = 0;
    uint8_t source = 0;
    uint32_t fix_time = 0;  // UTC seconds
    template <class A>
    void fields(A& a) { a.i32(lat_e7); a.i32(lon_e7); a.u16(accuracy_m); a.u8(source); a.u32(fix_time); }
};

struct WeatherRequest {
    SWP_MSG(WeatherRequest);
    template <class A>
    void fields(A&) {}
};

enum class PrecipKind : uint8_t { None = 0, Rain = 1, Snow = 2, Mixed = 3, Unknown = 0xFF };

struct WeatherDay {
    uint32_t date = 0;  // UTC midnight of the local day, seconds
    uint8_t wmo_code = 0;
    int16_t low_c10 = 0;
    int16_t high_c10 = 0;
    uint8_t precip_prob = 0;  // %
    uint16_t precip_mm10 = 0;
    uint16_t uv_max10 = 0;
    template <class A>
    void fields(A& a) {
        a.u32(date); a.u8(wmo_code); a.i16(low_c10); a.i16(high_c10); a.u8(precip_prob);
        a.u16(precip_mm10); a.u16(uv_max10);
    }
};

struct WeatherHour {
    uint32_t time = 0;
    uint8_t wmo_code = 0;
    int16_t temp_c10 = 0;
    uint8_t precip_prob = 0;
    template <class A>
    void fields(A& a) { a.u32(time); a.u8(wmo_code); a.i16(temp_c10); a.u8(precip_prob); }
};

struct Weather {
    SWP_MSG(Weather);
    uint32_t updated_at = 0;  // UTC seconds of the upstream observation
    uint8_t wmo_code = 0;
    std::string description;  // "raining", "clear", ...
    int16_t temp_c10 = 0;
    int16_t low_c10 = 0;
    int16_t high_c10 = 0;
    uint16_t wind_kmh10 = 0;
    uint16_t uv10 = 0;
    uint8_t precip_kind = static_cast<uint8_t>(PrecipKind::Unknown);  // next expected precipitation
    int32_t precip_in_min = -1;  // minutes until precipitation; 0 = now; -1 = none forecast/unknown
    uint8_t precip_horizon_h = 0;  // forecast window the "none" statement covers
    std::vector<WeatherDay> days;
    std::vector<WeatherHour> hours;
    template <class A>
    void fields(A& a) {
        a.u32(updated_at); a.u8(wmo_code); a.str8(description); a.i16(temp_c10); a.i16(low_c10);
        a.i16(high_c10); a.u16(wind_kmh10); a.u16(uv10); a.u8(precip_kind); a.i32(precip_in_min);
        a.u8(precip_horizon_h); a.list(days, 7); a.list(hours, 24);
    }
};

struct MapTileRequest {
    SWP_MSG(MapTileRequest);
    uint8_t z = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t resume_offset = 0;  // non-zero to resume an interrupted transfer
    template <class A>
    void fields(A& a) { a.u8(z); a.u32(x); a.u32(y); a.u32(resume_offset); }
};

struct MapSearchRequest {
    SWP_MSG(MapSearchRequest);
    std::string query;  // empty: nearby POIs
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
    uint8_t limit = 10;
    template <class A>
    void fields(A& a) { a.str8(query); a.i32(lat_e7); a.i32(lon_e7); a.u8(limit); }
};

struct Place {
    std::string name;
    std::string category;
    std::string address;
    int32_t lat_e7 = 0;
    int32_t lon_e7 = 0;
    uint32_t distance_m = 0;
    template <class A>
    void fields(A& a) {
        a.str8(name); a.str8(category); a.str8(address); a.i32(lat_e7); a.i32(lon_e7); a.u32(distance_m);
    }
};

struct MapSearchResults {
    SWP_MSG(MapSearchResults);
    std::string query;
    std::vector<Place> places;
    template <class A>
    void fields(A& a) { a.str8(query); a.list(places, 20); }
};

enum class TravelMode : uint8_t { Walk = 0, Bike = 1, Drive = 2 };

struct RouteRequest {
    SWP_MSG(RouteRequest);
    int32_t from_lat_e7 = 0, from_lon_e7 = 0, to_lat_e7 = 0, to_lon_e7 = 0;
    uint8_t mode = 0;
    template <class A>
    void fields(A& a) { a.i32(from_lat_e7); a.i32(from_lon_e7); a.i32(to_lat_e7); a.i32(to_lon_e7); a.u8(mode); }
};

struct RoutePoint {
    int32_t lat_e7 = 0, lon_e7 = 0;
    template <class A>
    void fields(A& a) { a.i32(lat_e7); a.i32(lon_e7); }
};

struct RouteStep {
    std::string instruction;
    uint32_t distance_m = 0;
    uint16_t point_index = 0;
    template <class A>
    void fields(A& a) { a.str8(instruction); a.u32(distance_m); a.u16(point_index); }
};

struct Route {
    SWP_MSG(Route);
    uint8_t status = 0;  // 0 ok, otherwise ErrorCode
    uint32_t distance_m = 0;
    uint32_t duration_s = 0;
    std::vector<RoutePoint> points;  // simplified server-side
    std::vector<RouteStep> steps;
    template <class A>
    void fields(A& a) {
        a.u8(status); a.u32(distance_m); a.u32(duration_s); a.list(points, 256); a.list(steps, 32);
    }
};

// Offline maps: the companion downloads all tiles around a point into its SD cache.
struct MapSyncRequest {
    SWP_MSG(MapSyncRequest);
    int32_t lat_e7 = 0, lon_e7 = 0;
    uint16_t radius_m = 3000;
    uint8_t zmin = 12, zmax = 17;
    template <class A>
    void fields(A& a) { a.i32(lat_e7); a.i32(lon_e7); a.u16(radius_m); a.u8(zmin); a.u8(zmax); }
};

enum class MapSyncState : uint8_t { Idle = 0, Running = 1, Done = 2, Failed = 3, Preparing = 4 };

struct MapSyncStatus {
    SWP_MSG(MapSyncStatus);
    uint8_t state = 0;  // MapSyncState
    uint32_t done = 0;  // tiles stored
    uint32_t kbytes = 0;
    std::string message;
    template <class A>
    void fields(A& a) { a.u8(state); a.u32(done); a.u32(kbytes); a.str8(message); }
};

// OSM data on the VPS helper (downloaded Geofabrik extracts and the map areas built from them).
enum class MapDataAction : uint8_t { Status = 0, Update = 1, Delete = 2 };

struct MapDataCommand {
    SWP_MSG(MapDataCommand);
    uint8_t action = 0;  // MapDataAction
    int32_t lat_e7 = 0, lon_e7 = 0;  // Update: area to rebuild
    template <class A>
    void fields(A& a) { a.u8(action); a.i32(lat_e7); a.i32(lon_e7); }
};

struct MapDataStatus {
    SWP_MSG(MapDataStatus);
    uint8_t status = 0;      // 0 ok, otherwise ErrorCode
    std::string active;      // area id, empty if none
    std::string building;    // area id being built, empty if idle
    std::string error;       // last build error or request failure
    template <class A>
    void fields(A& a) { a.u8(status); a.str8(active); a.str8(building); a.str16(error, 300); }
};

enum class TransferKind : uint8_t { MapTile = 1, AlbumArt = 2, Generic = 3 };

struct TransferBegin {
    SWP_MSG(TransferBegin);
    uint16_t id = 0;
    uint8_t kind = 0;
    uint32_t total_size = 0;
    uint32_t crc32 = 0;
    uint16_t chunk_size = 0;
    uint32_t start_offset = 0;  // non-zero when resuming
    std::vector<uint8_t> meta;  // kind-specific key, e.g. tile z/x/y
    template <class A>
    void fields(A& a) {
        a.u16(id); a.u8(kind); a.u32(total_size); a.u32(crc32); a.u16(chunk_size); a.u32(start_offset);
        a.bytes16(meta, 64);
    }
};

struct TransferChunk {
    SWP_MSG(TransferChunk);
    uint16_t id = 0;
    uint32_t offset = 0;
    std::vector<uint8_t> data;
    template <class A>
    void fields(A& a) { a.u16(id); a.u32(offset); a.bytes16(data, kMaxPayload - 16); }
};

enum class TransferAckStatus : uint8_t {
    Progress = 0,  // everything below next_offset received
    Done = 1,      // complete and CRC verified
    Rewind = 2,    // gap or CRC failure: resend from next_offset
};

struct TransferAck {
    SWP_MSG(TransferAck);
    uint16_t id = 0;
    uint32_t next_offset = 0;  // everything below has been received in order
    uint8_t window = 0;        // chunks the receiver can accept beyond next_offset
    uint8_t status = 0;        // TransferAckStatus
    template <class A>
    void fields(A& a) { a.u16(id); a.u32(next_offset); a.u8(window); a.u8(status); }
};

struct TransferAbort {
    SWP_MSG(TransferAbort);
    uint16_t id = 0;
    uint8_t reason = 0;  // ErrorCode
    template <class A>
    void fields(A& a) { a.u16(id); a.u8(reason); }
};

struct Notification {
    SWP_MSG(Notification);
    uint64_t id = 0;
    uint32_t timestamp = 0;  // UTC seconds
    uint8_t priority = 3;    // ntfy priority 1..5
    std::string topic;
    std::string title;
    std::string body;
    template <class A>
    void fields(A& a) {
        a.u64(id); a.u32(timestamp); a.u8(priority); a.str8(topic); a.str8(title); a.str16(body, 1024);
    }
};

struct NotificationAck {
    SWP_MSG(NotificationAck);
    uint64_t id = 0;
    template <class A>
    void fields(A& a) { a.u64(id); }
};

struct PlaylistsRequest {
    SWP_MSG(PlaylistsRequest);
    template <class A>
    void fields(A&) {}
};

struct PlaylistInfo {
    std::string id;
    std::string name;
    uint16_t track_count = 0;
    uint32_t duration_s = 0;
    uint8_t synced = 0;  // 1 = all tracks available offline
    template <class A>
    void fields(A& a) { a.str8(id); a.str8(name); a.u16(track_count); a.u32(duration_s); a.u8(synced); }
};

struct Playlists {
    SWP_MSG(Playlists);
    std::vector<PlaylistInfo> playlists;
    template <class A>
    void fields(A& a) { a.list(playlists, 32); }
};

struct PlaylistRequest {
    SWP_MSG(PlaylistRequest);
    std::string playlist_id;
    uint16_t offset = 0;
    uint16_t count = 20;
    template <class A>
    void fields(A& a) { a.str8(playlist_id); a.u16(offset); a.u16(count); }
};

struct TrackInfo {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    uint32_t duration_s = 0;
    uint16_t index = 0;
    uint8_t available = 0;
    template <class A>
    void fields(A& a) {
        a.str8(id); a.str8(title); a.str8(artist); a.str8(album); a.u32(duration_s); a.u16(index); a.u8(available);
    }
};

struct PlaylistTracks {
    SWP_MSG(PlaylistTracks);
    std::string playlist_id;
    uint16_t total = 0;
    uint16_t offset = 0;
    std::vector<TrackInfo> tracks;
    template <class A>
    void fields(A& a) { a.str8(playlist_id); a.u16(total); a.u16(offset); a.list(tracks, 24); }
};

enum class PlaybackCmd : uint8_t {
    PlayPlaylist = 1,  // str = playlist id, arg = track index
    Pause = 2, Resume = 3, Toggle = 4, Next = 5, Previous = 6,
    Seek = 7,          // arg = position ms
    SetShuffle = 8,    // arg = 0/1
    SetRepeat = 9,     // arg = RepeatMode
    Stop = 10,
    SetVolume = 11,    // arg = 0..127 (AVRCP absolute volume)
    AlertTone = 12,    // short alert tone on the connected sink (timer/alarm), arg = repeats
};

enum class RepeatMode : uint8_t { Off = 0, All = 1, One = 2 };
enum class PlayState : uint8_t { Stopped = 0, Playing = 1, Paused = 2, Buffering = 3, Error = 4 };

struct PlaybackCommand {
    SWP_MSG(PlaybackCommand);
    uint8_t cmd = 0;
    uint32_t arg = 0;
    std::string str;
    template <class A>
    void fields(A& a) { a.u8(cmd); a.u32(arg); a.str8(str); }
};

struct PlaybackStatus {
    SWP_MSG(PlaybackStatus);
    uint8_t state = 0;
    std::string playlist_id;
    uint16_t index = 0;
    std::string track_id;
    std::string title;
    std::string artist;
    std::string album;
    uint32_t position_ms = 0;
    uint32_t duration_ms = 0;
    uint8_t shuffle = 0;
    uint8_t repeat = 0;
    uint8_t volume = 0;
    uint8_t sink_connected = 0;
    std::string error;
    template <class A>
    void fields(A& a) {
        a.u8(state); a.str8(playlist_id); a.u16(index); a.str8(track_id); a.str8(title); a.str8(artist);
        a.str8(album); a.u32(position_ms); a.u32(duration_ms); a.u8(shuffle); a.u8(repeat); a.u8(volume);
        a.u8(sink_connected); a.str8(error);
    }
};

struct WifiConfigSet {
    SWP_MSG(WifiConfigSet);
    std::string ssid;
    std::string password;  // write-only; never echoed back
    template <class A>
    void fields(A& a) { a.str8(ssid); a.str8(password); }
};

struct WifiStatusRequest {
    SWP_MSG(WifiStatusRequest);
    template <class A>
    void fields(A&) {}
};

enum class NetState : uint8_t { Disabled = 0, NotConfigured = 1, Searching = 2, Connecting = 3, Connected = 4, Failed = 5 };
enum class NetProvider : uint8_t { None = 0, Wifi = 1, Lte = 2, Phone = 3 };

struct WifiStatus {
    SWP_MSG(WifiStatus);
    uint8_t state = 0;
    uint8_t provider = 0;  // active internet provider
    std::string ssid;
    int8_t rssi = 0;
    uint32_t ipv4 = 0;  // network byte order packed as a.b.c.d -> a | b<<8 | ...
    uint8_t password_set = 0;
    uint8_t internet_ok = 0;  // last request to the VPS helper succeeded
    template <class A>
    void fields(A& a) {
        a.u8(state); a.u8(provider); a.str8(ssid); a.i8(rssi); a.u32(ipv4); a.u8(password_set); a.u8(internet_ok);
    }
};

struct AudioScanStart {
    SWP_MSG(AudioScanStart);
    uint8_t seconds = 10;
    template <class A>
    void fields(A& a) { a.u8(seconds); }
};

struct AudioDevice {
    uint8_t bda[6] = {};
    std::string name;
    int8_t rssi = 0;
    uint32_t cod = 0;
    uint8_t remembered = 0;
    template <class A>
    void fields(A& a) { a.fixed(bda); a.str8(name); a.i8(rssi); a.u32(cod); a.u8(remembered); }
};

struct AudioDevices {
    SWP_MSG(AudioDevices);
    uint8_t scanning = 0;
    std::vector<AudioDevice> devices;
    template <class A>
    void fields(A& a) { a.u8(scanning); a.list(devices, 16); }
};

struct AudioConnect {
    SWP_MSG(AudioConnect);
    uint8_t bda[6] = {};
    template <class A>
    void fields(A& a) { a.fixed(bda); }
};

struct AudioDisconnect {
    SWP_MSG(AudioDisconnect);
    template <class A>
    void fields(A&) {}
};

struct AudioForget {
    SWP_MSG(AudioForget);
    uint8_t bda[6] = {};
    template <class A>
    void fields(A& a) { a.fixed(bda); }
};

enum class SinkState : uint8_t { Idle = 0, Discovering = 1, Connecting = 2, Connected = 3, Streaming = 4, Disconnecting = 5, Failed = 6 };

struct AudioStatus {
    SWP_MSG(AudioStatus);
    uint8_t state = 0;
    uint8_t bda[6] = {};
    std::string name;
    uint8_t remembered = 0;
    template <class A>
    void fields(A& a) { a.u8(state); a.fixed(bda); a.str8(name); a.u8(remembered); }
};

// Generic key/value configuration. Keys are whitelisted per device; secret keys are
// write-only (ConfigValue reports only whether they are set).
struct ConfigSet {
    SWP_MSG(ConfigSet);
    std::string key;
    std::string value;
    template <class A>
    void fields(A& a) { a.str8(key); a.str16(value, 512); }
};

struct ConfigGet {
    SWP_MSG(ConfigGet);
    std::string key;  // empty: list all keys
    template <class A>
    void fields(A& a) { a.str8(key); }
};

struct ConfigValue {
    SWP_MSG(ConfigValue);
    std::string key;
    uint8_t is_set = 0;
    uint8_t secret = 0;
    std::string value;  // empty for secrets
    uint8_t status = 0;  // ErrorCode on failure
    template <class A>
    void fields(A& a) { a.str8(key); a.u8(is_set); a.u8(secret); a.str16(value, 512); a.u8(status); }
};

// Watch -> companion: open (seconds > 0) or close (0) the LAN settings page
// (http://<companion-ip>/). The companion answers with WebConfigStatus, and again on change.
struct WebConfigCommand {
    SWP_MSG(WebConfigCommand);
    uint16_t seconds = 0;
    template <class A>
    void fields(A& a) { a.u16(seconds); }
};

struct WebConfigStatus {
    SWP_MSG(WebConfigStatus);
    uint8_t open = 0;
    uint16_t remaining_s = 0;
    std::string url;  // empty while the companion has no IP address
    template <class A>
    void fields(A& a) { a.u8(open); a.u16(remaining_s); a.str8(url); }
};

struct StatusRequest {
    SWP_MSG(StatusRequest);
    template <class A>
    void fields(A&) {}
};

enum class SdState : uint8_t { Absent = 0, Mounted = 1, Error = 2, Formatting = 3, Unsupported = 4 };
enum class SyncState : uint8_t { Idle = 0, Running = 1, Done = 2, Failed = 3, Paused = 4, NotConfigured = 5 };

struct CompanionStatus {
    SWP_MSG(CompanionStatus);
    std::string firmware;
    uint32_t uptime_s = 0;
    uint8_t sd_state = 0;
    uint32_t sd_total_mb = 0;
    uint32_t sd_free_mb = 0;
    uint32_t music_mb = 0;
    uint32_t maps_mb = 0;
    uint8_t net_state = 0;
    uint8_t net_provider = 0;
    uint8_t server_ok = 0;
    uint8_t sync_state = 0;
    uint16_t sync_done = 0;
    uint16_t sync_total = 0;
    uint32_t heap_free = 0;
    uint32_t psram_free = 0;
    uint16_t notif_pending = 0;
    uint32_t last_notif_poll = 0;  // UTC seconds
    template <class A>
    void fields(A& a) {
        a.str8(firmware); a.u32(uptime_s); a.u8(sd_state); a.u32(sd_total_mb); a.u32(sd_free_mb);
        a.u32(music_mb); a.u32(maps_mb); a.u8(net_state); a.u8(net_provider); a.u8(server_ok);
        a.u8(sync_state); a.u16(sync_done); a.u16(sync_total); a.u32(heap_free); a.u32(psram_free);
        a.u16(notif_pending); a.u32(last_notif_poll);
    }
};

struct DeviceInfo {
    SWP_MSG(DeviceInfo);
    uint8_t role = 0;
    std::string name;
    std::string firmware;
    std::string hardware;
    uint8_t mac[6] = {};
    uint8_t proto_version = kProtocolVersion;
    template <class A>
    void fields(A& a) { a.u8(role); a.str8(name); a.str8(firmware); a.str8(hardware); a.fixed(mac); a.u8(proto_version); }
};

struct WatchSettings {
    SWP_MSG(WatchSettings);
    uint8_t ntfy_enabled = 1;
    uint8_t vibration_enabled = 1;
    uint8_t brightness = 80;      // %
    uint8_t dim_brightness = 15;  // %
    uint16_t dim_after_s = 15;
    std::string posix_tz = "EST5EDT,M3.2.0,M11.1.0";
    uint16_t step_threshold_mg = 1150;
    template <class A>
    void fields(A& a) {
        a.u8(ntfy_enabled); a.u8(vibration_enabled); a.u8(brightness); a.u8(dim_brightness); a.u16(dim_after_s);
        a.str8(posix_tz); a.u16(step_threshold_mg);
    }
};


#undef SWP_MSG

// Serializes `msg` into a payload (without frame header).
template <class M>
std::vector<uint8_t> encode_payload(const M& msg) {
    std::vector<uint8_t> out;
    Writer w(out);
    EncodeVisitor v{w};
    const_cast<M&>(msg).fields(v);
    return out;
}

// Serializes `msg` into a complete frame. Returns false if it exceeds kMaxPayload.
template <class M>
bool encode_message(const M& msg, uint16_t seq, std::vector<uint8_t>& frame) {
    return encode_frame(static_cast<uint8_t>(M::kType), seq, encode_payload(msg), frame);
}

template <class M>
std::optional<M> decode_payload(const std::vector<uint8_t>& payload) {
    M m;
    Reader r(payload.data(), payload.size());
    DecodeVisitor v{r};
    m.fields(v);
    if (r.failed()) return std::nullopt;
    return m;
}

template <class M>
std::optional<M> decode_message(const Frame& f) {
    if (f.type != static_cast<uint8_t>(M::kType)) return std::nullopt;
    return decode_payload<M>(f.payload);
}

}  // namespace swp
