#include "core/notification_store.hpp"

#include <algorithm>

#include "swp/wire.hpp"

namespace core {

bool NotificationStore::seen(uint64_t id) const { return std::find(seen_.begin(), seen_.end(), id) != seen_.end(); }

bool NotificationStore::add(const StoredNotification& n) {
    if (seen(n.id)) return false;
    seen_.push_back(n.id);
    while (seen_.size() > max_seen_) seen_.pop_front();
    items_.push_front(n);
    while (items_.size() > max_items_) items_.pop_back();
    return true;
}

bool NotificationStore::dismiss(uint64_t id) {
    auto it = std::find_if(items_.begin(), items_.end(), [id](const StoredNotification& n) { return n.id == id; });
    if (it == items_.end()) return false;
    items_.erase(it);
    return true;
}

void NotificationStore::clear() { items_.clear(); }

void NotificationStore::mark_all_read() {
    for (auto& n : items_) n.read = true;
}

size_t NotificationStore::unread() const {
    return static_cast<size_t>(std::count_if(items_.begin(), items_.end(), [](const StoredNotification& n) { return !n.read; }));
}

std::vector<uint8_t> NotificationStore::serialize() const {
    std::vector<uint8_t> out;
    swp::Writer w(out);
    w.u8(1);  // format version
    w.u16(static_cast<uint16_t>(items_.size()));
    for (auto& n : items_) {
        w.u64(n.id);
        w.u32(n.timestamp);
        w.u8(n.priority);
        w.boolean(n.read);
        w.str8(n.title);
        w.str16(n.body, 1024);
    }
    w.u16(static_cast<uint16_t>(seen_.size()));
    for (auto id : seen_) w.u64(id);
    return out;
}

bool NotificationStore::deserialize(const std::vector<uint8_t>& data) {
    swp::Reader r(data.data(), data.size());
    if (r.u8() != 1) return false;
    std::deque<StoredNotification> items;
    uint16_t n = r.u16();
    if (n > max_items_) return false;
    for (uint16_t i = 0; i < n && !r.failed(); ++i) {
        StoredNotification s;
        s.id = r.u64();
        s.timestamp = r.u32();
        s.priority = r.u8();
        s.read = r.boolean();
        s.title = r.str8();
        s.body = r.str16(1024);
        items.push_back(std::move(s));
    }
    std::deque<uint64_t> seen;
    uint16_t m = r.u16();
    if (m > max_seen_) return false;
    for (uint16_t i = 0; i < m && !r.failed(); ++i) seen.push_back(r.u64());
    if (r.failed()) return false;
    items_ = std::move(items);
    seen_ = std::move(seen);
    return true;
}

}  // namespace core
