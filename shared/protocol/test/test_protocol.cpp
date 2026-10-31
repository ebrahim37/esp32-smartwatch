#include <cstring>
#include <random>

#include "minitest.hpp"
#include "swp/messages.hpp"
#include "swp/transfer.hpp"

using namespace swp;

TEST(crc_known_vectors) {
    const char* s = "123456789";
    CHECK_EQ(crc16_ccitt(reinterpret_cast<const uint8_t*>(s), 9), 0x29B1);
    CHECK_EQ(crc32(reinterpret_cast<const uint8_t*>(s), 9), 0xCBF43926u);
}

TEST(wire_little_endian) {
    std::vector<uint8_t> out;
    Writer w(out);
    w.u16(0x1234);
    w.u32(0xA1B2C3D4);
    w.i16(-2);
    CHECK_EQ(out.size(), 8u);
    CHECK_EQ(out[0], 0x34);
    CHECK_EQ(out[2], 0xD4);
    CHECK_EQ(out[5], 0xA1);
    CHECK_EQ(out[6], 0xFE);
    Reader r(out.data(), out.size());
    CHECK_EQ(r.u16(), 0x1234);
    CHECK_EQ(r.u32(), 0xA1B2C3D4u);
    CHECK_EQ(r.i16(), -2);
    CHECK(r.at_end());
    r.u8();
    CHECK(r.failed());
}

TEST(utf8_truncation_keeps_code_points) {
    std::string s(254, 'a');
    s += "\xC3\xA9";  // é straddles the 255-byte limit
    std::vector<uint8_t> out;
    Writer w(out);
    w.str8(s);
    CHECK_EQ(out[0], 254);
    CHECK_EQ(utf8_safe_prefix("\xE2\x82\xAC", 2), 0u);
}

TEST(message_roundtrip_weather) {
    Weather wx;
    wx.description = "raining";
    wx.temp_c10 = 100;
    wx.low_c10 = 50;
    wx.high_c10 = 150;
    wx.wind_kmh10 = 130;
    wx.uv10 = 60;
    wx.precip_kind = static_cast<uint8_t>(PrecipKind::Rain);
    wx.precip_in_min = 240;
    wx.days.resize(3);
    wx.days[2].high_c10 = -45;
    wx.hours.resize(12);
    std::vector<uint8_t> frame;
    CHECK(encode_message(wx, 7, frame));
    FrameDecoder d;
    d.feed(frame.data(), frame.size());
    Frame f;
    CHECK(d.next(f) == FrameDecoder::Result::Frame);
    CHECK_EQ(f.seq, 7);
    auto back = decode_message<Weather>(f);
    CHECK(back.has_value());
    CHECK_EQ(back->description, std::string("raining"));
    CHECK_EQ(back->precip_in_min, 240);
    CHECK_EQ(back->days.size(), 3u);
    CHECK_EQ(back->days[2].high_c10, -45);
    CHECK_EQ(back->hours.size(), 12u);
    CHECK(!decode_message<Notification>(f).has_value());
}

TEST(message_roundtrip_map_sync_and_data) {
    MapSyncRequest r;
    r.lat_e7 = 437800833;
    r.lon_e7 = -794118611;
    r.radius_m = 5000;
    r.zmax = 16;
    std::vector<uint8_t> frame;
    CHECK(encode_message(r, 1, frame));
    FrameDecoder d;
    d.feed(frame.data(), frame.size());
    Frame f;
    CHECK(d.next(f) == FrameDecoder::Result::Frame);
    auto back = decode_message<MapSyncRequest>(f);
    CHECK(back.has_value() && back->lon_e7 == -794118611 && back->radius_m == 5000 && back->zmin == 12 && back->zmax == 16);

    MapDataStatus st;
    st.active = "+43.8_-79.4_100km";
    st.error = std::string(400, 'e');  // longer than the field allows: truncated, not rejected
    frame.clear();
    CHECK(encode_message(st, 2, frame));
    d.feed(frame.data(), frame.size());
    CHECK(d.next(f) == FrameDecoder::Result::Frame);
    auto s2 = decode_message<MapDataStatus>(f);
    CHECK(s2.has_value() && s2->active == st.active && s2->error.size() <= 300);
}

TEST(decode_rejects_truncated_and_oversized_lists) {
    MapSearchResults res;
    res.places.resize(3);
    res.places[0].name = "Cafe";
    auto p = encode_payload(res);
    for (size_t cut = 0; cut < p.size(); ++cut) {
        std::vector<uint8_t> t(p.begin(), p.begin() + static_cast<long>(cut));
        CHECK(!decode_payload<MapSearchResults>(t).has_value());
    }
    // Claim 1000 places: exceeds max_count -> rejected without allocating.
    std::vector<uint8_t> bad = {0, 0xE8, 0x03};
    CHECK(!decode_payload<MapSearchResults>(bad).has_value());
}

TEST(forward_compatible_trailing_bytes) {
    Heartbeat hb;
    hb.uptime_s = 42;
    auto p = encode_payload(hb);
    p.push_back(0x99);  // field from a future minor version
    auto back = decode_payload<Heartbeat>(p);
    CHECK(back.has_value());
    CHECK_EQ(back->uptime_s, 42u);
}

TEST(frame_decoder_fragmented_and_resync) {
    Notification n;
    n.id = 0x1122334455667788ull;
    n.title = "Door";
    n.body = std::string(600, 'x');
    std::vector<uint8_t> f1, f2;
    encode_message(n, 1, f1);
    Heartbeat hb;
    encode_message(hb, 2, f2);
    std::vector<uint8_t> stream = {0x00, 0xA5, 0x13};  // garbage incl. a stray magic byte
    stream.insert(stream.end(), f1.begin(), f1.end());
    std::vector<uint8_t> corrupt = f2;
    corrupt[9] ^= 0xFF;
    stream.insert(stream.end(), corrupt.begin(), corrupt.end());
    stream.insert(stream.end(), f2.begin(), f2.end());
    FrameDecoder d;
    std::vector<Frame> got;
    for (size_t i = 0; i < stream.size(); i += 20) {
        d.feed(stream.data() + i, std::min<size_t>(20, stream.size() - i));
        Frame f;
        FrameDecoder::Result r;
        while ((r = d.next(f)) != FrameDecoder::Result::NeedMore)
            if (r == FrameDecoder::Result::Frame) got.push_back(f);
    }
    CHECK_EQ(got.size(), 2u);
    CHECK_EQ(d.crc_errors(), 1u);
    auto back = decode_message<Notification>(got[0]);
    CHECK(back && back->id == n.id && back->body.size() == 600);
    CHECK_EQ(got[1].seq, 2);
}

TEST(frame_rejects_oversized_payload) {
    std::vector<uint8_t> big(kMaxPayload + 1), out;
    CHECK(!encode_frame(1, 0, big, out));
}

static std::vector<uint8_t> random_blob(size_t n, unsigned seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    return v;
}

// Drives sender/receiver through an in-memory link; `drop_every` loses every Nth chunk.
static bool run_transfer(const std::vector<uint8_t>& blob, int drop_every, int* rounds_out) {
    TransferSender tx;
    TransferReceiver rx(1 << 20, 6);
    tx.start(9, TransferKind::MapTile, blob.data(), static_cast<uint32_t>(blob.size()), 180, 0, {1, 2, 3});
    TransferAck ack;
    rx.on_begin(tx.begin_message(), ack);
    tx.on_ack(ack);
    int sent = 0, rounds = 0;
    bool complete = false;
    while (!complete && rounds < 100000) {
        ++rounds;
        auto c = tx.next_chunk();
        if (!c) {
            // Sender blocked: simulate the receiver's periodic ack (timeout path).
            TransferAck a;
            a.id = 9;
            a.next_offset = rx.received();
            a.window = 6;
            a.status = static_cast<uint8_t>(TransferAckStatus::Rewind);
            tx.on_ack(a);
            continue;
        }
        ++sent;
        if (drop_every && sent % drop_every == 0) continue;
        bool send_ack = false;
        auto ev = rx.on_chunk(*c, ack, send_ack);
        if (send_ack) tx.on_ack(ack);
        if (ev == TransferReceiver::Event::Complete) complete = true;
    }
    if (rounds_out) *rounds_out = rounds;
    return complete && tx.done() && rx.data() == blob;
}

TEST(transfer_lossless) {
    auto blob = random_blob(20000, 1);
    CHECK(run_transfer(blob, 0, nullptr));
}

TEST(transfer_recovers_from_drops) {
    auto blob = random_blob(9001, 2);
    CHECK(run_transfer(blob, 7, nullptr));
}

TEST(transfer_resume_after_disconnect) {
    auto blob = random_blob(5000, 3);
    TransferSender tx;
    TransferReceiver rx(1 << 20, 4);
    tx.start(1, TransferKind::MapTile, blob.data(), 5000, 200, 0, {7});
    TransferAck ack;
    rx.on_begin(tx.begin_message(), ack);
    tx.on_ack(ack);
    bool send_ack;
    for (int i = 0; i < 3; ++i) {
        auto c = tx.next_chunk();
        rx.on_chunk(*c, ack, send_ack);
        if (send_ack) tx.on_ack(ack);
    }
    CHECK_EQ(rx.received(), 600u);
    // Link drops; a new sender resumes at the receiver's offset.
    TransferSender tx2;
    tx2.start(2, TransferKind::MapTile, blob.data(), 5000, 200, rx.received(), {7});
    rx.on_begin(tx2.begin_message(), ack);
    CHECK_EQ(ack.next_offset, 600u);
    tx2.on_ack(ack);
    TransferReceiver::Event ev = TransferReceiver::Event::None;
    while (ev != TransferReceiver::Event::Complete) {
        auto c = tx2.next_chunk();
        if (!c) break;
        ev = rx.on_chunk(*c, ack, send_ack);
        if (send_ack) tx2.on_ack(ack);
    }
    CHECK(ev == TransferReceiver::Event::Complete);
    CHECK(rx.data() == blob);
}

TEST(transfer_rejects_too_large) {
    TransferReceiver rx(1000);
    TransferBegin b;
    b.total_size = 1001;
    TransferAck ack;
    CHECK(rx.on_begin(b, ack) == TransferReceiver::Event::Failed);
    CHECK(!rx.active());
}

TEST(transfer_backpressure_respects_window) {
    auto blob = random_blob(10000, 4);
    TransferSender tx;
    tx.start(1, TransferKind::Generic, blob.data(), 10000, 100, 0, {});
    CHECK(!tx.next_chunk().has_value());  // no credit before the first ack
    TransferAck a;
    a.id = 1;
    a.window = 3;
    tx.on_ack(a);
    int n = 0;
    while (tx.next_chunk()) ++n;
    CHECK_EQ(n, 3);
}

int main() { return minitest::run_all(); }
