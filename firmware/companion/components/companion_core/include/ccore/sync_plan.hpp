// Music synchronisation planning: reconciles the selected playlist against what is on the
// SD card, honouring the music quota.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ccore {

struct RemoteTrack {
    std::string id;
    std::string suffix;  // file extension from the server ("mp3", "flac", "m4a")
    uint64_t size = 0;   // original file size as reported by Subsonic
};

struct LocalFile {
    std::string id;
    std::string suffix;
    uint64_t size = 0;
    bool partial = false;  // a .part download exists
};

struct SyncPlan {
    std::vector<std::string> download;        // ids to fetch (in playlist order)
    std::vector<std::string> resume;          // ids with a resumable partial download
    std::vector<std::string> remove;          // local ids no longer needed
    std::vector<std::string> over_quota;      // playlist tracks that will not fit
    std::vector<std::string> keep;            // already complete
    uint64_t bytes_needed = 0;
};

bool supported_suffix(const std::string& suffix);
std::string track_filename(const std::string& id, const std::string& suffix);

// Track ids are server-generated; anything outside [A-Za-z0-9_-] is rejected so a hostile
// server cannot escape /music with a path.
bool safe_id(const std::string& id);

SyncPlan plan_sync(const std::vector<RemoteTrack>& playlist, const std::vector<LocalFile>& local, uint64_t quota_bytes);

}  // namespace ccore
