// Bounded notification history with id-based deduplication (delivery is at-least-once).
#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace core {

struct StoredNotification {
    uint64_t id = 0;
    uint32_t timestamp = 0;
    uint8_t priority = 3;
    std::string title;
    std::string body;
    bool read = false;
};

class NotificationStore {
public:
    explicit NotificationStore(size_t max_items = 40, size_t max_seen = 256) : max_items_(max_items), max_seen_(max_seen) {}

    // Returns false if `n.id` was already received (duplicate redelivery).
    bool add(const StoredNotification& n);
    bool dismiss(uint64_t id);
    void clear();
    void mark_all_read();
    size_t unread() const;
    const std::deque<StoredNotification>& items() const { return items_; }  // newest first

    // Compact persistence (versioned, length-checked).
    std::vector<uint8_t> serialize() const;
    bool deserialize(const std::vector<uint8_t>& data);

private:
    bool seen(uint64_t id) const;
    size_t max_items_, max_seen_;
    std::deque<StoredNotification> items_;
    std::deque<uint64_t> seen_;
};

}  // namespace core
