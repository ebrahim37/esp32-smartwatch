// SWP framing over a BLE byte stream.
//
// A frame is written to the RX characteristic (watch -> peer) or notified on the TX
// characteristic (peer -> watch), split into ATT-MTU-sized pieces by the transport.
//
//   offset size  field
//   0      2     magic 0xA5 0x5A
//   2      1     protocol major version (kProtocolVersion)
//   3      1     message type (MsgType)
//   4      2     sequence number (LE)
//   6      2     payload length (LE, <= kMaxPayload)
//   8      N     payload
//   8+N    2     CRC-16/CCITT-FALSE over bytes [2, 8+N)
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace swp {

constexpr uint8_t kMagic0 = 0xA5;
constexpr uint8_t kMagic1 = 0x5A;
constexpr uint8_t kProtocolVersion = 1;
constexpr size_t kHeaderSize = 8;
constexpr size_t kCrcSize = 2;
constexpr size_t kMaxPayload = 4096;
constexpr size_t kMaxFrame = kHeaderSize + kMaxPayload + kCrcSize;

uint16_t crc16_ccitt(const uint8_t* data, size_t len, uint16_t crc = 0xFFFF);
uint32_t crc32(const uint8_t* data, size_t len, uint32_t crc = 0);

struct Frame {
    uint8_t version = kProtocolVersion;
    uint8_t type = 0;
    uint16_t seq = 0;
    std::vector<uint8_t> payload;
};

// Returns false if the payload exceeds kMaxPayload.
bool encode_frame(uint8_t type, uint16_t seq, const std::vector<uint8_t>& payload, std::vector<uint8_t>& out);

// Incremental decoder: feed arbitrary byte pieces, pull complete frames. Resynchronizes on
// the magic bytes after corruption, so one bad fragment cannot wedge the stream.
class FrameDecoder {
public:
    enum class Result { NeedMore, Frame, Error };

    // Appends bytes to the internal buffer. Returns false if the buffer would overflow
    // (the buffer is then reset).
    bool feed(const uint8_t* data, size_t len);

    // Extracts the next frame if one is complete. On Error the offending bytes are dropped
    // and the caller may call next() again.
    Result next(Frame& out);

    void reset() { buf_.clear(); }
    size_t buffered() const { return buf_.size(); }
    uint32_t crc_errors() const { return crc_errors_; }
    uint32_t version_errors() const { return version_errors_; }

private:
    std::vector<uint8_t> buf_;
    uint32_t crc_errors_ = 0;
    uint32_t version_errors_ = 0;
};

}  // namespace swp
