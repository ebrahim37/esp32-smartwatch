// ntfy subscription helpers: topic lists, NDJSON line splitting and an at-least-once resume
// cursor. The companion streams https://<ntfy>/<t1,t2>/json?since=<cursor> and only advances
// the cursor past messages the watch has acknowledged.
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ccore::ntfy {

constexpr size_t kMaxTopics = 16;
constexpr size_t kMaxTopicLen = 64;

// ntfy topic names: 1-64 of [A-Za-z0-9_-].
bool valid_topic(std::string_view t);
// Comma-separated list -> trimmed, valid, de-duplicated topics (at most kMaxTopics).
std::vector<std::string> split_topics(std::string_view csv);
std::string join_topics(const std::vector<std::string>& topics);
// True if every comma-separated entry is a valid topic and there are at most kMaxTopics.
bool valid_topic_list(std::string_view csv);

uint64_t fnv1a64(std::string_view s);
// Stable non-zero 63-bit key for an ntfy message id (SWP notification ids are u64).
inline uint64_t message_key(std::string_view ntfy_id) {
    uint64_t h = fnv1a64(ntfy_id) & 0x7FFFFFFFFFFFFFFFull;
    return h ? h : 1;
}

// Splits a byte stream into '\n'-terminated lines; overlong lines are dropped whole.
class LineSplitter {
public:
    explicit LineSplitter(size_t max_line = 8192) : max_(max_line) {}
    void feed(const char* data, size_t n, const std::function<void(std::string_view)>& on_line);
    void reset() { buf_.clear(), overflow_ = false; }

private:
    size_t max_;
    std::string buf_;
    bool overflow_ = false;
};

// Tracks delivered-but-unacknowledged messages in arrival order. The resume cursor is the
// newest message id such that it and everything before it has been acknowledged, so a
// reconnect (or reboot) with since=<cursor> replays anything the watch never confirmed.
class AckCursor {
public:
    explicit AckCursor(std::string cursor = {}) : cursor_(std::move(cursor)) {}
    // Returns false for a duplicate (still pending or recently acknowledged).
    bool add(const std::string& ntfy_id);
    // Marks a message acknowledged. Returns the new cursor if it advanced.
    std::optional<std::string> ack(uint64_t key);
    const std::string& cursor() const { return cursor_; }
    size_t pending() const;

private:
    struct Entry {
        std::string id;
        uint64_t key;
        bool acked;
    };
    std::deque<Entry> order_;
    std::deque<uint64_t> recent_;  // recently retired keys, for de-duplication on replay
    std::string cursor_;
};

}  // namespace ccore::ntfy
