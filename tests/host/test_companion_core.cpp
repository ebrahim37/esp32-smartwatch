#include <cstring>

#include "ccore/backoff.hpp"
#include "ccore/md5.hpp"
#include "ccore/ntfy.hpp"
#include "ccore/pcm.hpp"
#include "ccore/play_queue.hpp"
#include "ccore/quota.hpp"
#include "ccore/sd_policy.hpp"
#include "ccore/subsonic.hpp"
#include "ccore/sync_plan.hpp"
#include "minitest.hpp"

using namespace ccore;

static void make_fat32_vbr(uint8_t* s) {
    std::memset(s, 0, 512);
    s[0] = 0xEB; s[1] = 0x58; s[2] = 0x90;
    std::memcpy(s + 3, "MSDOS5.0", 8);
    s[11] = 0x00; s[12] = 0x02;  // 512 bytes/sector
    s[13] = 64;                  // sectors/cluster
    s[14] = 32;                  // reserved
    s[16] = 2;                   // FATs
    s[36] = 0x00; s[37] = 0x10;  // FAT32 size
    std::memcpy(s + 82, "FAT32   ", 8);
    s[510] = 0x55; s[511] = 0xAA;
}

static void make_mbr(uint8_t* s, uint8_t type, uint32_t lba) {
    std::memset(s, 0, 512);
    uint8_t* e = s + 446;
    e[4] = type;
    e[8] = lba & 0xFF; e[9] = (lba >> 8) & 0xFF;
    e[12] = 0x00; e[13] = 0x00; e[14] = 0xE0; e[15] = 0x00;
    s[510] = 0x55; s[511] = 0xAA;
}

TEST(sd_classifies_fat32_superfloppy_and_mbr) {
    uint8_t s[512];
    uint32_t lba; uint8_t type;
    make_fat32_vbr(s);
    CHECK(classify_sector0(s, lba, type) == VolumeKind::Fat32);
    CHECK_EQ(lba, 0u);
    make_mbr(s, 0x0C, 8192);
    CHECK(classify_sector0(s, lba, type) == VolumeKind::Fat32);
    CHECK_EQ(lba, 8192u);
    make_mbr(s, 0x83, 2048);
    CHECK(classify_sector0(s, lba, type) == VolumeKind::LinuxOrOther);
}

TEST(sd_classifies_exfat_ntfs_blank_unknown) {
    uint8_t s[512] = {};
    CHECK(classify_vbr(s) == VolumeKind::Blank);
    std::memset(s, 0xFF, 512);
    CHECK(sector_blank(s));
    std::memset(s, 0, 512);
    std::memcpy(s + 3, "EXFAT   ", 8);
    CHECK(classify_vbr(s) == VolumeKind::ExFat);
    std::memcpy(s + 3, "NTFS    ", 8);
    CHECK(classify_vbr(s) == VolumeKind::Ntfs);
    std::memset(s, 0, 512);
    s[100] = 0x42;  // random data, no signature
    uint32_t lba; uint8_t type;
    CHECK(classify_sector0(s, lba, type) == VolumeKind::Unknown);
}

TEST(sd_decision_never_formats_on_errors) {
    SdProbe p;
    CHECK(decide(p).action == SdAction::Refuse);  // absent
    p.card_present = true;
    p.kind = VolumeKind::Blank;
    p.read_ok = false;
    CHECK(decide(p).action == SdAction::Refuse);  // I/O failure, even if it "looked" blank
    p.read_ok = true;
    CHECK(decide(p).action == SdAction::Format);
    CHECK(!decide(p).destroys_data);
    p.kind = VolumeKind::Unknown;
    CHECK(decide(p).action == SdAction::Refuse);
    p.kind = VolumeKind::ExFat;
    CHECK(decide(p).action == SdAction::Format);
    CHECK(decide(p).destroys_data);
    p.kind = VolumeKind::Fat32;
    CHECK(decide(p).action == SdAction::Mount);
    CHECK(on_mount_failure().action == SdAction::Refuse);
}

TEST(quota_and_reserve) {
    Quota q{kMusicQuota, kMusicQuota - 100};
    CHECK(q.fits(100));
    CHECK(!q.fits(101));
    Quota m{kMusicQuota, 0};
    CHECK(!can_store(m, 10 * kMiB, 100 * kMiB));  // would cut into the free-space reserve
    CHECK(can_store(m, 10 * kMiB, 1 * kGiB));
}

TEST(lru_evicts_oldest_to_watermark) {
    LruIndex idx(1000);
    for (int i = 0; i < 10; ++i) idx.put("t" + std::to_string(i), 100, static_cast<uint64_t>(i));
    CHECK_EQ(idx.used(), 1000u);
    idx.touch("t0", 50);  // recently used: must survive
    auto ev = idx.plan_eviction(100, 90);
    // target = 900 - 100 = 800 -> evict 2 oldest (t1, t2), not t0
    CHECK_EQ(ev.size(), 2u);
    CHECK_EQ(ev[0], std::string("t1"));
    CHECK_EQ(ev[1], std::string("t2"));
    for (auto& k : ev) idx.remove(k);
    CHECK_EQ(idx.used(), 800u);
    CHECK(idx.plan_eviction(100).empty());
}

TEST(lru_incremental_blocks_and_free_bytes) {
    LruIndex idx(10000);
    idx.add("15/1/1", 300, 1);
    idx.add("15/1/1", 200, 5);   // second tile in the same block
    idx.add("15/2/1", 400, 3);
    CHECK_EQ(idx.used(), 900u);
    idx.add("15/1/1", -100, 6);  // overwrite with a smaller tile
    CHECK_EQ(idx.used(), 800u);
    auto f = idx.plan_free_bytes(350);
    CHECK_EQ(f.size(), 1u);
    CHECK_EQ(f[0], std::string("15/2/1"));
    CHECK_EQ(idx.entries().size(), 2u);
}

TEST(sync_plan_reconciles) {
    std::vector<RemoteTrack> pl = {
        {"a1", "mp3", 100}, {"b2", "flac", 200}, {"c3", "m4a", 300}, {"../evil", "mp3", 1}, {"d4", "ogg", 5}, {"e5", "mp3", 600}};
    std::vector<LocalFile> local = {{"a1", "mp3", 100, false}, {"b2", "flac", 50, true}, {"zz", "mp3", 10, false}};
    auto p = plan_sync(pl, local, 650);
    CHECK_EQ(p.keep.size(), 1u);
    CHECK_EQ(p.resume.size(), 1u);
    CHECK_EQ(p.resume[0], std::string("b2"));
    CHECK_EQ(p.download.size(), 1u);
    CHECK_EQ(p.download[0], std::string("c3"));
    CHECK_EQ(p.over_quota.size(), 1u);  // e5 exceeds the remaining budget
    CHECK_EQ(p.remove.size(), 1u);
    CHECK_EQ(p.remove[0], std::string("zz"));
    CHECK_EQ(p.bytes_needed, 150u + 300u);
    CHECK(!safe_id("a/b"));
    CHECK(safe_id("4kL9_-x"));
}

TEST(sync_plan_redownloads_size_mismatch) {
    auto p = plan_sync({{"a1", "mp3", 100}}, {{"a1", "mp3", 90, false}}, 1000);
    CHECK_EQ(p.download.size(), 1u);
}

TEST(md5_and_subsonic_token) {
    CHECK_EQ(md5_hex(""), std::string("d41d8cd98f00b204e9800998ecf8427e"));
    CHECK_EQ(md5_hex("The quick brown fox jumps over the lazy dog"), std::string("9e107d9d372bb6826bd81d3542a419d6"));
    // Example from the Subsonic API docs: password "sesame", salt "c19b2d".
    CHECK_EQ(md5_hex("sesamec19b2d"), std::string("26719a1196d2a940705a59634eb18eab"));
    SubsonicAuth a;
    a.user = "joe doe";
    a.password = "sesame";
    auto url = subsonic_url("https://music.example/", "getPlaylist", a, "c19b2d", "id=42");
    CHECK_EQ(url, std::string("https://music.example/rest/getPlaylist?u=joe%20doe&t=26719a1196d2a940705a59634eb18eab&s=c19b2d&v=1.16.1&c=esp32-companion&f=json&id=42"));
    CHECK(url.find("sesame") == std::string::npos);
}

TEST(backoff_grows_and_caps) {
    Backoff b(1000, 8000, 0);
    CHECK_EQ(b.next(0), 1000u);
    CHECK_EQ(b.next(0), 2000u);
    CHECK_EQ(b.next(0), 4000u);
    CHECK_EQ(b.next(0), 8000u);
    CHECK_EQ(b.next(0), 8000u);
    b.reset();
    CHECK_EQ(b.next(0), 1000u);
    Backoff j(1000, 60000, 20);
    uint32_t d = j.next(0xFFFFFFFF);
    CHECK(d >= 800 && d <= 1200);
}

TEST(play_queue_repeat_modes) {
    PlayQueue q;
    q.load(3, 1, false, 0);
    CHECK_EQ(q.current(), 1);
    CHECK(q.next(false));
    CHECK_EQ(q.current(), 2);
    CHECK(!q.next(false));  // repeat off: stop at end
    q.set_repeat(Repeat::All);
    CHECK(q.next(false));
    CHECK_EQ(q.current(), 0);
    q.set_repeat(Repeat::One);
    CHECK(q.next(false));
    CHECK_EQ(q.current(), 0);  // natural end repeats the track
    CHECK(q.next(true));
    CHECK_EQ(q.current(), 1);  // user Next still advances
    CHECK(q.prev(false));
    CHECK_EQ(q.current(), 0);
    CHECK(q.prev(true));
    CHECK_EQ(q.current(), 0);  // >3 s played: restart current
}

TEST(play_queue_shuffle_is_permutation_starting_with_selection) {
    PlayQueue q;
    q.load(50, 17, true, 1234);
    CHECK_EQ(q.current(), 17);
    std::vector<int> seen(50, 0);
    seen[q.current()]++;
    while (q.next(false)) seen[q.current()]++;
    for (int v : seen) CHECK_EQ(v, 1);
    q.load(10, 4, true, 99);
    q.next(true);
    int cur = q.current();
    q.set_shuffle(false, 0);
    CHECK_EQ(q.current(), cur);  // un-shuffling keeps the playing track
}

TEST(pcm_passthrough_and_mono_upmix) {
    PcmConverter c;
    CHECK(c.configure(44100, 2, 16));
    int16_t in[8] = {100, -100, 200, -200, 300, -300, 400, -400};
    int16_t out[16];
    size_t used = 0;
    size_t n = c.process(reinterpret_cast<uint8_t*>(in), sizeof in, out, 8, used);
    CHECK_EQ(n, 3u);  // one frame of lookahead is held back for interpolation
    CHECK_EQ(out[0], 100);
    CHECK_EQ(out[3], -200);
    CHECK_EQ(used, sizeof in);
    PcmConverter m;
    CHECK(m.configure(44100, 1, 16));
    int16_t mono[4] = {1000, 2000, 3000, 4000};
    n = m.process(reinterpret_cast<uint8_t*>(mono), sizeof mono, out, 8, used);
    CHECK_EQ(n, 3u);
    CHECK_EQ(out[2], 2000);
    CHECK_EQ(out[3], 2000);
}

TEST(pcm_resamples_48k_and_24bit) {
    PcmConverter c;
    CHECK(c.configure(48000, 2, 24));
    std::vector<uint8_t> in(4800 * 6);  // 100 ms of 24-bit stereo
    for (size_t f = 0; f < 4800; ++f) {
        int32_t v = 0x123456;  // positive constant -> 0x1234 in 16-bit
        for (int ch = 0; ch < 2; ++ch) {
            in[f * 6 + ch * 3] = v & 0xFF;
            in[f * 6 + ch * 3 + 1] = (v >> 8) & 0xFF;
            in[f * 6 + ch * 3 + 2] = (v >> 16) & 0xFF;
        }
    }
    std::vector<int16_t> out(2 * 5000);
    size_t used = 0;
    size_t n = c.process(in.data(), in.size(), out.data(), 5000, used);
    CHECK(n >= 4405 && n <= 4411);  // 100 ms at 44.1 kHz
    CHECK_EQ(out[100], 0x1234);
    CHECK(!c.configure(1000, 2, 16));
    CHECK(!c.configure(44100, 2, 12));
}

TEST(ntfy_topics) {
    using namespace ccore::ntfy;
    CHECK(valid_topic("alerts_1-x"));
    CHECK(!valid_topic(""));
    CHECK(!valid_topic("a/b"));
    CHECK(!valid_topic(std::string(65, 'a')));
    auto t = split_topics(" monitor, email,,bad topic,monitor ,omp");
    CHECK_EQ(t.size(), size_t(3));
    CHECK_EQ(join_topics(t), std::string("monitor,email,omp"));
    CHECK(valid_topic_list("a,b"));
    CHECK(valid_topic_list(""));
    CHECK(!valid_topic_list("a,b c"));
}

TEST(ntfy_line_splitter) {
    ccore::ntfy::LineSplitter ls(16);
    std::vector<std::string> lines;
    auto f = [&](std::string_view l) { lines.emplace_back(l); };
    ls.feed("{\"a\":1}\r\n{\"b\"", 13, f);
    ls.feed(":2}\n", 4, f);
    ls.feed("0123456789abcdefXYZ\nok\n", 23, f);  // first line too long: dropped whole
    CHECK_EQ(lines.size(), size_t(3));
    CHECK_EQ(lines[0], std::string("{\"a\":1}"));
    CHECK_EQ(lines[1], std::string("{\"b\":2}"));
    CHECK_EQ(lines[2], std::string("ok"));
}

TEST(ntfy_ack_cursor_only_advances_over_acked_prefix) {
    using namespace ccore::ntfy;
    AckCursor c("start");
    CHECK(c.add("m1"));
    CHECK(c.add("m2"));
    CHECK(c.add("m3"));
    CHECK(!c.add("m2"));                         // duplicate while pending
    CHECK(!c.ack(message_key("m2")).has_value());  // m1 still unacked: cursor stays
    CHECK_EQ(c.cursor(), std::string("start"));
    auto adv = c.ack(message_key("m1"));
    CHECK(adv.has_value() && *adv == "m2");      // m1 and m2 retired together
    CHECK(!c.add("m1"));                         // replay after reconnect is de-duplicated
    CHECK_EQ(c.pending(), size_t(1));
    CHECK(!c.ack(message_key("nope")).has_value());
}

int main() { return minitest::run_all(); }
