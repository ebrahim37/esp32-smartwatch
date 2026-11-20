// Offline music library (port of firmware/companion/main/music/library.cpp): one track table,
// playlists index into it. library.json keeps the companion's format. Songs live in the app's
// Documents directory, excluded from device backups.
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "app.hpp"

namespace paths {
namespace {

std::string make_dir(NSSearchPathDirectory base, NSString* sub, bool no_backup) {
    NSString* p = [NSSearchPathForDirectoriesInDomains(base, NSUserDomainMask, YES).firstObject stringByAppendingPathComponent:sub];
    [NSFileManager.defaultManager createDirectoryAtPath:p withIntermediateDirectories:YES attributes:nil error:nil];
    if (no_backup) {
        NSURL* u = [NSURL fileURLWithPath:p];
        [u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    }
    return str(p);
}

NSDictionary* fs_attrs() {
    return [NSFileManager.defaultManager attributesOfFileSystemForPath:ns(music()) error:nil];
}

}  // namespace

std::string music() {
    static std::string p = make_dir(NSDocumentDirectory, @"music", true);
    return p;
}
std::string memos() {
    static std::string p = make_dir(NSDocumentDirectory, @"memos", true);
    return p;
}
std::string maps() {
    static std::string p = make_dir(NSCachesDirectory, @"maps", false);
    return p;
}

uint64_t free_bytes() {
    NSNumber* n = nil;
    [[NSURL fileURLWithPath:ns(music())] getResourceValue:&n forKey:NSURLVolumeAvailableCapacityForImportantUsageKey error:nil];
    return n ? n.unsignedLongLongValue : [fs_attrs()[NSFileSystemFreeSize] unsignedLongLongValue];
}

uint64_t total_bytes() { return [fs_attrs()[NSFileSystemSize] unsignedLongLongValue]; }

}  // namespace paths

namespace library {
namespace {

std::mutex mu;
std::vector<Track> tracks;
std::vector<Playlist> lists;
std::vector<uint32_t> all_songs;
std::string search_id;
std::vector<uint32_t> search_hits;

std::string json_path() { return paths::music() + "/library.json"; }

// Case-folded for matching: ASCII lower case, and the Arabic Yeh/Kaf written as their Persian
// forms (both spellings are common in names).
std::string fold(const std::string& in) {
    std::string s;
    s.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        unsigned char c = in[i];
        if (c == 0xD9 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x8A) {
            s += "\xDB\x8C";  // U+064A -> U+06CC
            ++i;
        } else if (c == 0xD9 && i + 1 < in.size() && static_cast<unsigned char>(in[i + 1]) == 0x83) {
            s += "\xDA\xA9";  // U+0643 -> U+06A9
            ++i;
        } else {
            s += static_cast<char>(c < 0x80 ? std::tolower(c) : c);
        }
    }
    return s;
}

// Caller holds mu.
const std::vector<uint32_t>* indices(const std::string& id) {
    if (id == kAllSongsId) return &all_songs;
    if (id.rfind(kSearchPrefix, 0) == 0) {
        if (id != search_id) {
            search_id = id;
            search_hits.clear();
            std::string q = fold(id.substr(strlen(kSearchPrefix)));
            for (uint32_t i : all_songs)
                if (fold(tracks[i].title).find(q) != std::string::npos || fold(tracks[i].artist).find(q) != std::string::npos) search_hits.push_back(i);
        }
        return &search_hits;
    }
    for (auto& p : lists)
        if (p.id == id) return &p.tracks;
    return nullptr;
}

void rebuild_all_songs() {
    search_id.clear();
    all_songs.clear();
    for (uint32_t i = 0; i < tracks.size(); ++i)
        if (tracks[i].available) all_songs.push_back(i);
    std::sort(all_songs.begin(), all_songs.end(), [](uint32_t a, uint32_t b) { return strcasecmp(tracks[a].title.c_str(), tracks[b].title.c_str()) < 0; });
}

}  // namespace

void load() {
    NSData* raw = [NSData dataWithContentsOfFile:ns(json_path())];
    if (!raw || raw.length > 64 * 1024 * 1024) return;
    id root = [NSJSONSerialization JSONObjectWithData:raw options:0 error:nil];
    if (![jval(root, @"tracks") isKindOfClass:NSArray.class]) {
        NSLog(@"library.json unreadable; the next sync rebuilds it");
        return;
    }
    std::unordered_map<std::string, uint64_t> present;
    if (DIR* d = opendir(paths::music().c_str())) {
        while (struct dirent* e = readdir(d)) {
            struct stat st;
            std::string p = paths::music() + "/" + e->d_name;
            if (stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode)) present.emplace(e->d_name, st.st_size);
        }
        closedir(d);
    }
    std::vector<Track> ts;
    for (id t in jarr(root, @"tracks")) {
        Track tr;
        tr.id = jstr(t, @"id");
        tr.title = jstr(t, @"title");
        tr.artist = jstr(t, @"artist");
        tr.album = jstr(t, @"album");
        tr.suffix = jstr(t, @"suffix");
        tr.duration_s = static_cast<uint32_t>(std::max(jnum(t, @"duration"), 0.0));
        tr.size = static_cast<uint64_t>(std::max(jnum(t, @"size"), 0.0));
        auto f = present.find(tr.id + "." + tr.suffix);
        tr.available = tr.size > 0 && f != present.end() && f->second == tr.size;
        ts.push_back(std::move(tr));
    }
    std::vector<Playlist> ls;
    for (id pl in jarr(root, @"playlists")) {
        Playlist p;
        p.id = jstr(pl, @"id");
        p.name = jstr(pl, @"name");
        p.duration_s = static_cast<uint32_t>(std::max(jnum(pl, @"duration"), 0.0));
        for (id i in jarr(pl, @"tracks"))
            if ([i isKindOfClass:NSNumber.class] && [i doubleValue] >= 0 && [i doubleValue] < ts.size()) p.tracks.push_back([i unsignedIntValue]);
        ls.push_back(std::move(p));
    }
    std::lock_guard<std::mutex> g(mu);
    tracks = std::move(ts);
    lists = std::move(ls);
    rebuild_all_songs();
    NSLog(@"library: %zu tracks (%zu downloaded), %zu playlists", tracks.size(), all_songs.size(), lists.size());
}

bool save() {
    NSMutableArray* ta = [NSMutableArray array];
    NSMutableArray* pa = [NSMutableArray array];
    {
        std::lock_guard<std::mutex> g(mu);
        for (auto& t : tracks)
            [ta addObject:@{@"id": ns(t.id), @"title": ns(t.title), @"artist": ns(t.artist), @"album": ns(t.album), @"suffix": ns(t.suffix), @"duration": @(t.duration_s), @"size": @(t.size)}];
        for (auto& p : lists) {
            NSMutableArray* ia = [NSMutableArray arrayWithCapacity:p.tracks.size()];
            for (uint32_t i : p.tracks) [ia addObject:@(i)];
            [pa addObject:@{@"id": ns(p.id), @"name": ns(p.name), @"duration": @(p.duration_s), @"tracks": ia}];
        }
    }
    NSData* d = [NSJSONSerialization dataWithJSONObject:@{@"tracks": ta, @"playlists": pa} options:0 error:nil];
    return d && [d writeToFile:ns(json_path()) atomically:YES];
}

void replace(std::vector<Track> ts, std::vector<Playlist> ls) {
    std::lock_guard<std::mutex> g(mu);
    tracks = std::move(ts);
    lists = std::move(ls);
    rebuild_all_songs();
}

void set_available(const std::string& id, uint64_t size) {
    std::lock_guard<std::mutex> g(mu);
    for (auto& t : tracks)
        if (t.id == id) {
            t.size = size;
            t.available = size > 0;
        }
    rebuild_all_songs();
}

std::vector<swp::PlaylistInfo> playlists() {
    std::lock_guard<std::mutex> g(mu);
    std::vector<swp::PlaylistInfo> out;
    for (auto& p : lists) {
        swp::PlaylistInfo i;
        i.id = p.id;
        i.name = p.name;
        i.track_count = static_cast<uint16_t>(std::min<size_t>(p.tracks.size(), UINT16_MAX));
        bool all = !p.tracks.empty();
        for (uint32_t k : p.tracks) {
            i.duration_s += tracks[k].duration_s;
            all &= tracks[k].available;
        }
        i.synced = all;
        if (p.duration_s) i.duration_s = p.duration_s;
        out.push_back(i);
        if (out.size() >= 32) break;  // the message's limit
    }
    return out;
}

swp::PlaylistTracks page(const std::string& id, uint16_t offset, uint16_t count) {
    std::lock_guard<std::mutex> g(mu);
    swp::PlaylistTracks out;
    out.playlist_id = id;
    out.offset = offset;
    const auto* idx = indices(id);
    if (!idx) return out;
    out.total = static_cast<uint16_t>(std::min<size_t>(idx->size(), UINT16_MAX));
    count = std::min<uint16_t>(count, 12);
    for (size_t i = offset; i < out.total && out.tracks.size() < count; ++i) {
        const Track& t = tracks[(*idx)[i]];
        swp::TrackInfo ti;
        ti.id = t.id;
        ti.title = t.title;
        ti.artist = t.artist;
        ti.album = t.album;
        ti.duration_s = t.duration_s;
        ti.index = static_cast<uint16_t>(i);
        ti.available = t.available;
        out.tracks.push_back(std::move(ti));
    }
    return out;
}

std::vector<Track> tracks_of(const std::string& id) {
    std::lock_guard<std::mutex> g(mu);
    std::vector<Track> out;
    const auto* idx = indices(id);
    if (!idx) return out;
    out.reserve(std::min<size_t>(idx->size(), UINT16_MAX));
    for (uint32_t i : *idx) {
        if (out.size() >= UINT16_MAX) break;
        out.push_back(tracks[i]);
    }
    return out;
}

size_t downloaded() {
    std::lock_guard<std::mutex> g(mu);
    return all_songs.size();
}

std::string path_for(const std::string& id, const std::string& suffix) { return paths::music() + "/" + id + "." + suffix; }

}  // namespace library
