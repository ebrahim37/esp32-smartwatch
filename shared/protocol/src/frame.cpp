#include "swp/frame.hpp"

#include <algorithm>

namespace swp {

uint16_t crc16_ccitt(const uint8_t* data, size_t len, uint16_t crc) {
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b) crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021) : static_cast<uint16_t>(crc << 1);
    }
    return crc;
}

uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc) {
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

bool encode_frame(uint8_t type, uint16_t seq, const std::vector<uint8_t>& payload, std::vector<uint8_t>& out) {
    if (payload.size() > kMaxPayload) return false;
    out.clear();
    out.reserve(kHeaderSize + payload.size() + kCrcSize);
    out.push_back(kMagic0);
    out.push_back(kMagic1);
    out.push_back(kProtocolVersion);
    out.push_back(type);
    out.push_back(static_cast<uint8_t>(seq));
    out.push_back(static_cast<uint8_t>(seq >> 8));
    out.push_back(static_cast<uint8_t>(payload.size()));
    out.push_back(static_cast<uint8_t>(payload.size() >> 8));
    out.insert(out.end(), payload.begin(), payload.end());
    uint16_t crc = crc16_ccitt(out.data() + 2, out.size() - 2);
    out.push_back(static_cast<uint8_t>(crc));
    out.push_back(static_cast<uint8_t>(crc >> 8));
    return true;
}

bool FrameDecoder::feed(const uint8_t* data, size_t len) {
    if (buf_.size() + len > kMaxFrame * 2) {
        buf_.clear();
        return false;
    }
    buf_.insert(buf_.end(), data, data + len);
    return true;
}

FrameDecoder::Result FrameDecoder::next(Frame& out) {
    // Skip to the first magic sequence.
    size_t start = 0;
    while (start + 1 < buf_.size() && !(buf_[start] == kMagic0 && buf_[start + 1] == kMagic1)) ++start;
    // If no magic was found, `start` points at the last byte: keep it only if it could be the
    // first half of a magic pair split across fragments.
    if (start + 1 >= buf_.size() && !buf_.empty() && buf_.back() != kMagic0) start = buf_.size();
    if (start > 0) buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(start));
    if (buf_.size() < kHeaderSize) return Result::NeedMore;

    uint16_t len = static_cast<uint16_t>(buf_[6] | (buf_[7] << 8));
    if (len > kMaxPayload) {
        buf_.erase(buf_.begin(), buf_.begin() + 2);  // drop magic, resync
        return Result::Error;
    }
    size_t total = kHeaderSize + len + kCrcSize;
    if (buf_.size() < total) return Result::NeedMore;

    uint16_t want = static_cast<uint16_t>(buf_[total - 2] | (buf_[total - 1] << 8));
    uint16_t got = crc16_ccitt(buf_.data() + 2, total - 2 - kCrcSize);
    if (want != got) {
        ++crc_errors_;
        buf_.erase(buf_.begin(), buf_.begin() + 2);
        return Result::Error;
    }
    if (buf_[2] != kProtocolVersion) {
        ++version_errors_;
        buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(total));
        return Result::Error;
    }
    out.version = buf_[2];
    out.type = buf_[3];
    out.seq = static_cast<uint16_t>(buf_[4] | (buf_[5] << 8));
    out.payload.assign(buf_.begin() + kHeaderSize, buf_.begin() + kHeaderSize + len);
    buf_.erase(buf_.begin(), buf_.begin() + static_cast<long>(total));
    return Result::Frame;
}

}  // namespace swp
