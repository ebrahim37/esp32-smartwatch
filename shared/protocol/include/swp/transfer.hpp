// Chunked bulk transfer with ordering, CRC32 verification, credit-based backpressure and resume.
//
// Sender                                Receiver
//   TransferBegin(id,total,crc,start) ->
//                                     <- TransferAck(next_offset, window)
//   TransferChunk(offset..) x window  ->
//                                     <- TransferAck(next_offset, window)   (every window/2 chunks)
//   ...                               <- TransferAck(done=1)
//
// The receiver only accepts the chunk at exactly `next_offset` (go-back-N). A gap or
// duplicate triggers an immediate ack with the expected offset and the sender rewinds.
// A transfer interrupted by a disconnect can be resumed by re-requesting the object with
// `resume_offset`; the sender then begins at that offset and the receiver keeps its
// already-received prefix if `total_size` and `crc32` still match.
#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "swp/messages.hpp"

namespace swp {

class TransferSender {
public:
    // `data` must outlive the transfer.
    void start(uint16_t id, TransferKind kind, const uint8_t* data, uint32_t size, uint16_t chunk_size,
               uint32_t start_offset, std::vector<uint8_t> meta);
    TransferBegin begin_message() const;

    // Applies an ack from the receiver; returns false if the ack does not match this transfer.
    bool on_ack(const TransferAck& ack);

    // Next chunk allowed by the current credit window, or nullopt when blocked/done.
    std::optional<TransferChunk> next_chunk();

    bool active() const { return active_; }
    bool done() const { return done_; }
    uint16_t id() const { return id_; }
    uint32_t acked() const { return acked_; }
    void cancel() { active_ = false; }

private:
    const uint8_t* data_ = nullptr;
    uint32_t size_ = 0;
    uint32_t crc_ = 0;
    uint16_t chunk_ = 0;
    uint16_t id_ = 0;
    uint8_t kind_ = 0;
    uint32_t start_ = 0;
    std::vector<uint8_t> meta_;
    uint32_t send_offset_ = 0;  // next byte to send
    uint32_t acked_ = 0;        // receiver's next_offset
    uint8_t window_ = 0;        // chunks allowed beyond acked_
    bool active_ = false;
    bool done_ = false;
};

class TransferReceiver {
public:
    enum class Event { None, Progress, Complete, Failed };

    explicit TransferReceiver(uint32_t max_size = 256 * 1024, uint8_t window = 8) : max_size_(max_size), window_(window) {}

    // Handles TransferBegin; fills `ack` with the response to send.
    Event on_begin(const TransferBegin& b, TransferAck& ack);
    // Handles one chunk; `send_ack` is set when an ack should be sent now.
    Event on_chunk(const TransferChunk& c, TransferAck& ack, bool& send_ack);

    bool active() const { return active_; }
    uint16_t id() const { return id_; }
    uint8_t kind() const { return kind_; }
    const std::vector<uint8_t>& meta() const { return meta_; }
    const std::vector<uint8_t>& data() const { return buf_; }
    std::vector<uint8_t> take_data() { active_ = false; return std::move(buf_); }
    uint32_t received() const { return next_; }
    uint32_t total() const { return total_; }
    void reset() { active_ = false; buf_.clear(); next_ = 0; }

private:
    TransferAck make_ack(TransferAckStatus status) const;

    uint32_t max_size_;
    uint8_t window_;
    std::vector<uint8_t> buf_;
    std::vector<uint8_t> meta_;
    uint16_t id_ = 0;
    uint8_t kind_ = 0;
    uint32_t total_ = 0;
    uint32_t crc_ = 0;
    uint32_t next_ = 0;
    uint32_t since_ack_ = 0;
    bool active_ = false;
};

}  // namespace swp
