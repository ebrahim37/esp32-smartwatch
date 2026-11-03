#include "ccore/sync_plan.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <unordered_set>

namespace ccore {

bool supported_suffix(const std::string& s) {
    std::string l = s;
    std::transform(l.begin(), l.end(), l.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return l == "mp3" || l == "flac" || l == "m4a" || l == "mp4" || l == "aac";
}

bool safe_id(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    return std::all_of(id.begin(), id.end(), [](unsigned char c) { return std::isalnum(c) || c == '-' || c == '_'; });
}

std::string track_filename(const std::string& id, const std::string& suffix) { return id + "." + suffix; }

SyncPlan plan_sync(const std::vector<RemoteTrack>& playlist, const std::vector<LocalFile>& local, uint64_t quota) {
    SyncPlan p;
    std::unordered_map<std::string, const LocalFile*> have;
    for (auto& f : local) have[f.id] = &f;
    std::unordered_set<std::string> wanted;
    uint64_t budget = 0;
    for (auto& t : playlist) {
        if (!safe_id(t.id) || !supported_suffix(t.suffix) || wanted.count(t.id)) continue;
        uint64_t need = t.size ? t.size : t.budget;
        if (budget + need > quota) {
            p.over_quota.push_back(t.id);
            continue;
        }
        budget += need;
        wanted.insert(t.id);
        auto it = have.find(t.id);
        bool complete = it != have.end() && !it->second->partial && it->second->suffix == t.suffix &&
                        (t.size == 0 || it->second->size == t.size);
        if (complete) {
            p.keep.push_back(t.id);
        } else if (t.size > 0 && it != have.end() && it->second->partial && it->second->size < t.size) {
            p.resume.push_back(t.id);
            p.bytes_needed += t.size - it->second->size;
        } else {
            p.download.push_back(t.id);  // unknown size: a partial can't be resumed, start over
            p.bytes_needed += need;
        }
    }
    for (auto& f : local)
        if (!wanted.count(f.id)) p.remove.push_back(f.id);
    return p;
}

}  // namespace ccore
