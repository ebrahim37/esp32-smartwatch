// Voice memos: records the current input (the headphones' microphone over Bluetooth when connected,
// otherwise the phone's) to 16 kHz mono WAV under Documents/memos. Same names as the companion.
#import <AVFoundation/AVFoundation.h>

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <map>

#include "app.hpp"
#include "ccore/quota.hpp"
#include "ccore/sync_plan.hpp"

@interface SWMemoDelegate : NSObject <AVAudioRecorderDelegate>
@end

namespace memos {
namespace {

SWMemoDelegate* delegate;
AVAudioRecorder* recorder;
swp::MemoState state = swp::MemoState::Idle;
std::string error;
std::string cur_name;
double ended_at = -1e9;
NSTimer* timer;
std::map<std::string, uint32_t> durations;  // ms, by name; files don't change after recording

struct Entry {
    std::string name;
    uint32_t created = 0, duration_ms = 0;
    uint64_t size = 0;
};

std::string path_of(const std::string& name) { return paths::memos() + "/" + name + ".wav"; }

uint32_t duration_of(const std::string& name) {
    auto it = durations.find(name);
    if (it != durations.end()) return it->second;
    AVAudioPlayer* p = [[AVAudioPlayer alloc] initWithContentsOfURL:[NSURL fileURLWithPath:ns(path_of(name))] error:nil];
    uint32_t ms = p ? static_cast<uint32_t>(p.duration * 1000) : 0;
    durations[name] = ms;
    return ms;
}

std::vector<Entry> scan() {
    std::vector<Entry> out;
    DIR* d = opendir(paths::memos().c_str());
    if (!d) return out;
    while (struct dirent* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() < 5 || n.compare(n.size() - 4, 4, ".wav") != 0) continue;
        Entry m;
        m.name = n.substr(0, n.size() - 4);
        if (!ccore::safe_id(m.name) || (recorder && m.name == cur_name)) continue;
        struct stat st;
        if (stat(path_of(m.name).c_str(), &st) != 0) continue;
        m.size = static_cast<uint64_t>(st.st_size);
        m.duration_ms = duration_of(m.name);
        struct tm lt = {};
        if (sscanf(m.name.c_str(), "%4d%2d%2d-%2d%2d%2d", &lt.tm_year, &lt.tm_mon, &lt.tm_mday, &lt.tm_hour, &lt.tm_min, &lt.tm_sec) == 6) {
            lt.tm_year -= 1900;
            lt.tm_mon -= 1;
            lt.tm_isdst = -1;
            m.created = static_cast<uint32_t>(mktime(&lt));
        }
        out.push_back(std::move(m));
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.name > b.name; });
    return out;
}

void publish() { ble::send(status()); }

void set_state(swp::MemoState s, const std::string& err = "") {
    state = s;
    error = err;
    publish();
}

void finish_session() {
    [timer invalidate];
    timer = nil;
    player::apply_session();
}

void begin_recording() {
    if (state != swp::MemoState::Connecting) return;  // stopped while the permission prompt was up
    AVAudioSession* s = AVAudioSession.sharedInstance;
    NSError* err = nil;
    [s setCategory:AVAudioSessionCategoryPlayAndRecord mode:AVAudioSessionModeDefault options:AVAudioSessionCategoryOptionAllowBluetooth error:&err];
    [s setPreferredSampleRate:16000 error:nil];
    [s setActive:YES error:&err];
    for (AVAudioSessionPortDescription* in in s.availableInputs)
        if ([in.portType isEqualToString:AVAudioSessionPortBluetoothHFP]) [s setPreferredInput:in error:nil];
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char name[24];
    strftime(name, sizeof name, "%Y%m%d-%H%M%S", &lt);
    cur_name = name;
    NSDictionary* settings = @{AVFormatIDKey: @(kAudioFormatLinearPCM), AVSampleRateKey: @16000.0, AVNumberOfChannelsKey: @1,
                               AVLinearPCMBitDepthKey: @16, AVLinearPCMIsFloatKey: @NO, AVLinearPCMIsBigEndianKey: @NO};
    recorder = [[AVAudioRecorder alloc] initWithURL:[NSURL fileURLWithPath:ns(path_of(cur_name))] settings:settings error:&err];
    recorder.delegate = delegate;
    if (!recorder || ![recorder record]) {
        NSLog(@"recorder: %@", err);
        recorder = nil;
        state = swp::MemoState::Error;  // finish_session restores audio only once this isn't active
        finish_session();
        return set_state(swp::MemoState::Error, "Could not start recording");
    }
    timer = [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer*) { publish(); }];
    set_state(swp::MemoState::Recording);
}

void start() {
    if (state == swp::MemoState::Connecting || state == swp::MemoState::Recording) return;
    uint64_t used = 0;
    for (auto& e : scan()) used += e.size;
    if (used >= ccore::kMemosQuota) return set_state(swp::MemoState::Error, "Memo storage full");
    if (!delegate) delegate = [SWMemoDelegate new];
    player::pause_for_memo();
    set_state(swp::MemoState::Connecting);
    [AVAudioSession.sharedInstance requestRecordPermission:^(BOOL granted) {
        dispatch_async(dispatch_get_main_queue(), ^{
            if (granted) begin_recording();
            else if (state == swp::MemoState::Connecting) set_state(swp::MemoState::Error, "Microphone permission denied");
        });
    }];
}

void stop() {
    if (state == swp::MemoState::Connecting) return set_state(swp::MemoState::Idle);
    if (state != swp::MemoState::Recording) return;
    ended_at = mono_s();
    [recorder stop];  // audioRecorderDidFinishRecording finishes the job
}

}  // namespace

void on_recorder_finished() {
    if (!recorder) return;
    recorder = nil;
    state = swp::MemoState::Idle;
    error.clear();
    durations.erase(cur_name);
    if (duration_of(cur_name) == 0) {  // nothing was captured: don't keep an empty memo
        unlink(path_of(cur_name).c_str());
        durations.erase(cur_name);
    }
    finish_session();
    set_state(swp::MemoState::Idle);
    ble::send(list());
}

void command(const swp::MemoCommand& c) {
    switch (static_cast<swp::MemoOp>(c.op)) {
        case swp::MemoOp::Start: start(); break;
        case swp::MemoOp::Stop: stop(); break;
        case swp::MemoOp::List: ble::send(list()); break;
        case swp::MemoOp::Delete:
            if (ccore::safe_id(c.name) && unlink(path_of(c.name).c_str()) == 0) durations.erase(c.name);
            ble::send(list());
            break;
    }
    publish();
}

swp::MemoStatus status() {
    swp::MemoStatus s;
    s.state = static_cast<uint8_t>(state);
    s.error = error;
    if (state == swp::MemoState::Recording) s.elapsed_ms = static_cast<uint32_t>(recorder.currentTime * 1000);
    return s;
}

swp::MemoList list() {
    swp::MemoList l;
    for (auto& e : scan()) {
        if (l.memos.size() >= 40) break;
        l.memos.push_back({e.name, e.created, e.duration_ms});
    }
    return l;
}

std::vector<library::Track> tracks() {
    std::vector<library::Track> out;
    for (auto& e : scan()) {
        if (out.size() >= 40) break;
        library::Track t;
        t.id = e.name;
        char title[40];
        struct tm lt = {};
        time_t c = e.created;
        localtime_r(&c, &lt);
        strftime(title, sizeof title, "Memo %b %d, %H:%M", &lt);
        t.title = title;
        t.artist = "Voice memo";
        t.suffix = "wav";
        t.duration_s = std::max<uint32_t>(1, e.duration_ms / 1000);
        t.size = e.size;
        t.available = true;
        t.path = path_of(e.name);
        out.push_back(std::move(t));
    }
    return out;
}

bool active() { return state == swp::MemoState::Connecting || state == swp::MemoState::Recording || recorder != nil; }

bool take_button() {
    if (state == swp::MemoState::Connecting || state == swp::MemoState::Recording) {
        stop();
        publish();
        return true;
    }
    return mono_s() - ended_at < 4;  // the press that stopped it, delivered late
}

}  // namespace memos

@implementation SWMemoDelegate
- (void)audioRecorderDidFinishRecording:(AVAudioRecorder*)r successfully:(BOOL)ok {
    dispatch_async(dispatch_get_main_queue(), ^{ memos::on_recorder_finished(); });
}
- (void)audioRecorderEncodeErrorDidOccur:(AVAudioRecorder*)r error:(NSError*)e {
    NSLog(@"recorder error: %@", e);
    [r stop];
}
@end
