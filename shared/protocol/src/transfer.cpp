#include "swp/transfer.hpp"

#include <algorithm>

namespace swp {

void TransferSender::start(uint16_t id, TransferKind kind, const uint8_t* data, uint32_t size, uint16_t chunk_size,
                           uint32_t start_offset, std::vector<uint8_t> meta) {
    data_ = data;
    size_ = size;
    crc_ = crc32(data, size);
    chunk_ = chunk_size == 0 ? 180 : chunk_size;
    id_ = id;
    kind_ = static_cast<uint8_t>(kind);
    start_ = start_offset > size ? 0 : start_offset;
    meta_ = std::move(meta);
    send_offset_ = start_;
    acked_ = start_;
    window_ = 0;  // wait for the receiver's first ack before streaming
    active_ = true;
    done_ = false;
}

TransferBegin TransferSender::begin_message() const {
    TransferBegin b;
    b.id = id_;
    b.kind = kind_;
    b.total_size = size_;
    b.crc32 = crc_;
    b.chunk_size = chunk_;
    b.start_offset = start_;
    b.meta = meta_;
    return b;
}

bool TransferSender::on_ack(const TransferAck& ack) {
    if (!active_ || ack.id != id_) return false;
    if (ack.status == static_cast<uint8_t>(TransferAckStatus::Done)) {
        done_ = true;
        active_ = false;
        acked_ = size_;
        return true;
    }
    acked_ = std::min(ack.next_offset, size_);
    window_ = ack.window;
    // Go-back-N: on a rewind (or a stale send position) restart from the receiver's offset.
    if (ack.status == static_cast<uint8_t>(TransferAckStatus::Rewind) || send_offset_ < acked_) send_offset_ = acked_;
    return true;
}

std::optional<TransferChunk> TransferSender::next_chunk() {
    if (!active_ || send_offset_ >= size_) return std::nullopt;
    uint64_t limit = static_cast<uint64_t>(acked_) + static_cast<uint64_t>(window_) * chunk_;
    if (send_offset_ >= limit) return std::nullopt;
    TransferChunk c;
    c.id = id_;
    c.offset = send_offset_;
    uint32_t n = std::min<uint32_t>(chunk_, size_ - send_offset_);
    c.data.assign(data_ + send_offset_, data_ + send_offset_ + n);
    send_offset_ += n;
    return c;
}

TransferAck TransferReceiver::make_ack(TransferAckStatus status) const {
    TransferAck a;
    a.id = id_;
    a.next_offset = next_;
    a.window = window_;
    a.status = static_cast<uint8_t>(status);
    return a;
}

TransferReceiver::Event TransferReceiver::on_begin(const TransferBegin& b, TransferAck& ack) {
    kind_ = b.kind;  // valid for the caller even when this begin is rejected
    if (b.total_size > max_size_) {
        active_ = false;
        ack = TransferAck{};
        ack.id = b.id;
        ack.window = 0;
        return Event::Failed;
    }
    bool resumable = active_ && b.total_size == total_ && b.crc32 == crc_ && b.start_offset <= next_ && b.meta == meta_;
    if (!resumable) {
        buf_.clear();
        buf_.reserve(b.total_size);
        next_ = 0;
    }
    id_ = b.id;
    total_ = b.total_size;
    crc_ = b.crc32;
    meta_ = b.meta;
    since_ack_ = 0;
    active_ = true;
    if (total_ == 0) {
        bool ok = crc32(nullptr, 0) == crc_;
        ack = make_ack(ok ? TransferAckStatus::Done : TransferAckStatus::Rewind);
        active_ = false;
        return ok ? Event::Complete : Event::Failed;
    }
    ack = make_ack(TransferAckStatus::Progress);
    return Event::Progress;
}

TransferReceiver::Event TransferReceiver::on_chunk(const TransferChunk& c, TransferAck& ack, bool& send_ack) {
    send_ack = false;
    if (!active_ || c.id != id_) return Event::None;
    if (c.offset != next_ || c.data.empty() || next_ + c.data.size() > total_) {
        // Gap or oversized chunk: ask the sender to rewind. Duplicates are dropped silently.
        ack = make_ack(TransferAckStatus::Rewind);
        send_ack = c.offset > next_ || c.offset == next_;
        return Event::None;
    }
    buf_.insert(buf_.end(), c.data.begin(), c.data.end());
    next_ += static_cast<uint32_t>(c.data.size());
    ++since_ack_;
    if (next_ == total_) {
        bool ok = crc32(buf_.data(), buf_.size()) == crc_;
        send_ack = true;
        if (!ok) {
            // Corrupt: restart from zero.
            buf_.clear();
            next_ = 0;
            ack = make_ack(TransferAckStatus::Rewind);
            return Event::Failed;
        }
        active_ = false;
        ack = make_ack(TransferAckStatus::Done);
        return Event::Complete;
    }
    if (since_ack_ >= std::max<uint32_t>(1, window_ / 2)) {
        since_ack_ = 0;
        ack = make_ack(TransferAckStatus::Progress);
        send_ack = true;
    }
    return Event::Progress;
}

}  // namespace swp
