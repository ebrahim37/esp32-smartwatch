// Explicit little-endian serialization primitives for the Smartwatch Protocol (SWP).
// No struct memcpy: every field is written byte by byte so the encoding is ABI-independent.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace swp {

class Writer {
public:
    explicit Writer(std::vector<uint8_t>& out) : out_(out) {}

    void u8(uint8_t v) { out_.push_back(v); }
    void u16(uint16_t v) { put_le(v, 2); }
    void u32(uint32_t v) { put_le(v, 4); }
    void u64(uint64_t v) { put_le(v, 8); }
    void i8(int8_t v) { u8(static_cast<uint8_t>(v)); }
    void i16(int16_t v) { u16(static_cast<uint16_t>(v)); }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void raw(const uint8_t* data, size_t len) { out_.insert(out_.end(), data, data + len); }

    // Strings are truncated on a UTF-8 code point boundary so receivers never see split sequences.
    void str8(const std::string& s) { str_prefixed(s, 0xFF, 1); }
    void str16(const std::string& s, size_t max_len = 0xFFFF) { str_prefixed(s, max_len, 2); }
    void bytes16(const std::vector<uint8_t>& b, size_t max_len = 0xFFFF);

    size_t size() const { return out_.size(); }

private:
    void put_le(uint64_t v, int n) {
        for (int i = 0; i < n; ++i) out_.push_back(static_cast<uint8_t>(v >> (8 * i)));
    }
    void str_prefixed(const std::string& s, size_t max_len, int prefix_bytes);

    std::vector<uint8_t>& out_;
};

class Reader {
public:
    Reader(const uint8_t* data, size_t len) : p_(data), end_(data + len) {}

    uint8_t u8() { return static_cast<uint8_t>(get_le(1)); }
    uint16_t u16() { return static_cast<uint16_t>(get_le(2)); }
    uint32_t u32() { return static_cast<uint32_t>(get_le(4)); }
    uint64_t u64() { return get_le(8); }
    int8_t i8() { return static_cast<int8_t>(u8()); }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    bool boolean() { return u8() != 0; }
    std::string str8();
    std::string str16(size_t max_len = 0xFFFF);
    std::vector<uint8_t> bytes16(size_t max_len = 0xFFFF);

    size_t remaining() const { return failed_ ? 0 : static_cast<size_t>(end_ - p_); }
    bool at_end() const { return remaining() == 0; }
    bool failed() const { return failed_; }
    void fail() { failed_ = true; }

private:
    uint64_t get_le(int n);
    bool need(size_t n) {
        if (failed_ || static_cast<size_t>(end_ - p_) < n) {
            failed_ = true;
            return false;
        }
        return true;
    }

    const uint8_t* p_;
    const uint8_t* end_;
    bool failed_ = false;
};

// Symmetric field visitors: each message defines `template <class A> void fields(A& a)` once,
// and the same description drives both encoding and decoding.
struct EncodeVisitor {
    Writer& w;
    bool more() const { return true; }
    void u8(uint8_t& v) { w.u8(v); }
    void u16(uint16_t& v) { w.u16(v); }
    void u32(uint32_t& v) { w.u32(v); }
    void u64(uint64_t& v) { w.u64(v); }
    void i8(int8_t& v) { w.i8(v); }
    void i16(int16_t& v) { w.i16(v); }
    void i32(int32_t& v) { w.i32(v); }
    void boolean(bool& v) { w.boolean(v); }
    void str8(std::string& s) { w.str8(s); }
    void str16(std::string& s, size_t max_len = 0xFFFF) { w.str16(s, max_len); }
    void bytes16(std::vector<uint8_t>& b, size_t max_len = 0xFFFF) { w.bytes16(b, max_len); }
    template <class T>
    void list(std::vector<T>& v, size_t max_count) {
        size_t n = v.size() < max_count ? v.size() : max_count;
        w.u16(static_cast<uint16_t>(n));
        for (size_t i = 0; i < n; ++i) v[i].fields(*this);
    }
    template <size_t N>
    void fixed(uint8_t (&arr)[N]) { w.raw(arr, N); }
};

struct DecodeVisitor {
    Reader& r;
    // Fields appended in later protocol minor versions are guarded with `if (!a.more()) return;`
    // so a newer decoder accepts payloads from an older encoder.
    bool more() const { return !r.at_end(); }
    void u8(uint8_t& v) { v = r.u8(); }
    void u16(uint16_t& v) { v = r.u16(); }
    void u32(uint32_t& v) { v = r.u32(); }
    void u64(uint64_t& v) { v = r.u64(); }
    void i8(int8_t& v) { v = r.i8(); }
    void i16(int16_t& v) { v = r.i16(); }
    void i32(int32_t& v) { v = r.i32(); }
    void boolean(bool& v) { v = r.boolean(); }
    void str8(std::string& s) { s = r.str8(); }
    void str16(std::string& s, size_t max_len = 0xFFFF) { s = r.str16(max_len); }
    void bytes16(std::vector<uint8_t>& b, size_t max_len = 0xFFFF) { b = r.bytes16(max_len); }
    template <class T>
    void list(std::vector<T>& v, size_t max_count) {
        uint16_t n = r.u16();
        if (n > max_count || r.failed()) {
            r.fail();
            return;
        }
        v.clear();
        v.resize(n);
        for (auto& item : v) {
            item.fields(*this);
            if (r.failed()) return;
        }
    }
    template <size_t N>
    void fixed(uint8_t (&arr)[N]) {
        for (size_t i = 0; i < N; ++i) arr[i] = r.u8();
    }
};

// Length of the longest prefix of `s` that is at most `max_len` bytes and ends on a code point.
size_t utf8_safe_prefix(const std::string& s, size_t max_len);

}  // namespace swp
