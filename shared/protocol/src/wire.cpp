#include "swp/wire.hpp"

namespace swp {

size_t utf8_safe_prefix(const std::string& s, size_t max_len) {
    if (s.size() <= max_len) return s.size();
    size_t n = max_len;
    // Back up over continuation bytes (10xxxxxx) so the cut lands before a lead byte.
    while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

void Writer::str_prefixed(const std::string& s, size_t max_len, int prefix_bytes) {
    size_t limit = prefix_bytes == 1 ? (max_len < 0xFF ? max_len : 0xFF) : (max_len < 0xFFFF ? max_len : 0xFFFF);
    size_t n = utf8_safe_prefix(s, limit);
    if (prefix_bytes == 1)
        u8(static_cast<uint8_t>(n));
    else
        u16(static_cast<uint16_t>(n));
    raw(reinterpret_cast<const uint8_t*>(s.data()), n);
}

void Writer::bytes16(const std::vector<uint8_t>& b, size_t max_len) {
    size_t n = b.size();
    if (n > max_len) n = max_len;
    if (n > 0xFFFF) n = 0xFFFF;
    u16(static_cast<uint16_t>(n));
    raw(b.data(), n);
}

uint64_t Reader::get_le(int n) {
    if (!need(static_cast<size_t>(n))) return 0;
    uint64_t v = 0;
    for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(p_[i]) << (8 * i);
    p_ += n;
    return v;
}

std::string Reader::str8() {
    uint8_t n = u8();
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return s;
}

std::string Reader::str16(size_t max_len) {
    uint16_t n = u16();
    if (n > max_len) {
        fail();
        return {};
    }
    if (!need(n)) return {};
    std::string s(reinterpret_cast<const char*>(p_), n);
    p_ += n;
    return s;
}

std::vector<uint8_t> Reader::bytes16(size_t max_len) {
    uint16_t n = u16();
    if (n > max_len) {
        fail();
        return {};
    }
    if (!need(n)) return {};
    std::vector<uint8_t> b(p_, p_ + n);
    p_ += n;
    return b;
}

}  // namespace swp
