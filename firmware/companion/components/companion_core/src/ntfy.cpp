#include "ccore/ntfy.hpp"

#include <algorithm>

namespace ccore::ntfy {

bool valid_topic(std::string_view t) {
    if (t.empty() || t.size() > kMaxTopicLen) return false;
    for (char c : t)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}

namespace {
std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}
template <class F>
void for_each_entry(std::string_view csv, F f) {
    while (!csv.empty()) {
        size_t comma = csv.find(',');
        f(trim(csv.substr(0, comma)));
        if (comma == std::string_view::npos) break;
        csv.remove_prefix(comma + 1);
    }
}
}  // namespace

std::vector<std::string> split_topics(std::string_view csv) {
    std::vector<std::string> out;
    for_each_entry(csv, [&](std::string_view t) {
        if (out.size() < kMaxTopics && valid_topic(t) && std::find(out.begin(), out.end(), t) == out.end()) out.emplace_back(t);
    });
    return out;
}

std::string join_topics(const std::vector<std::string>& topics) {
    std::string s;
    for (auto& t : topics) s += (s.empty() ? "" : ",") + t;
    return s;
}

bool valid_topic_list(std::string_view csv) {
    if (trim(csv).empty()) return true;  // empty list: subscription off
    size_t n = 0;
    bool ok = true;
    for_each_entry(csv, [&](std::string_view t) {
        ++n;
        ok = ok && valid_topic(t);
    });
    return ok && n <= kMaxTopics;
}

uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

void LineSplitter::feed(const char* data, size_t n, const std::function<void(std::string_view)>& on_line) {
    for (size_t i = 0; i < n; ++i) {
        char c = data[i];
        if (c == '\n') {
            if (!overflow_ && !buf_.empty()) on_line(buf_);
            buf_.clear();
            overflow_ = false;
        } else if (!overflow_) {
            if (buf_.size() >= max_) {
                overflow_ = true;
                buf_.clear();
            } else if (c != '\r') {
                buf_.push_back(c);
            }
        }
    }
}

bool AckCursor::add(const std::string& ntfy_id) {
    uint64_t key = message_key(ntfy_id);
    if (std::find(recent_.begin(), recent_.end(), key) != recent_.end()) return false;
    for (auto& e : order_)
        if (e.key == key) return false;
    order_.push_back({ntfy_id, key, false});
    return true;
}

std::optional<std::string> AckCursor::ack(uint64_t key) {
    bool found = false;
    for (auto& e : order_)
        if (e.key == key) {
            e.acked = true;
            found = true;
        }
    if (!found) return std::nullopt;
    bool advanced = false;
    while (!order_.empty() && order_.front().acked) {
        cursor_ = order_.front().id;
        recent_.push_back(order_.front().key);
        if (recent_.size() > 64) recent_.pop_front();
        order_.pop_front();
        advanced = true;
    }
    if (!advanced) return std::nullopt;
    return cursor_;
}

size_t AckCursor::pending() const {
    size_t n = 0;
    for (auto& e : order_) n += !e.acked;
    return n;
}

}  // namespace ccore::ntfy
