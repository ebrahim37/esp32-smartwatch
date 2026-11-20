// Music playback through the phone's audio route (port of the command handling in
// firmware/companion/main/music/player.cpp; AVAudioEngine replaces the decoder and A2DP). The
// queue plays a snapshot of the playlist, so downloads finishing meanwhile don't shift it.
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <MediaPlayer/MediaPlayer.h>
#import <UIKit/UIKit.h>

#include <algorithm>
#include <cmath>

#include "app.hpp"
#include "ccore/play_queue.hpp"

@interface SWPlayerDelegate : NSObject <AVAudioPlayerDelegate>
@end

// One track on its own engine. Volume above 1 is boost through a peak limiter.
@interface SWTrack : NSObject
@property(nonatomic, readonly) double duration;
@property(nonatomic) double currentTime;
@property(nonatomic) float volume;
- (instancetype)initWithPath:(NSString*)path error:(NSError**)err;
- (BOOL)play;
- (void)pause;
- (void)stop;
@end

namespace player {

NSData* wav_data(const std::vector<int16_t>& pcm, int rate);
namespace {

using swp::PlaybackCmd;

NSString* const kVolumeKey = @"player.volume";
NSString* const kKeepAliveKey = @"player.keep_alive";

SWPlayerDelegate* delegate;
SWTrack* ap;
AVAudioPlayer* alert_player;
AVAudioPlayer* silence;
swp::PlaybackStatus st;
ccore::PlayQueue order;
std::string playlist;
std::vector<library::Track> queue_tracks;
library::Track cur;
bool playing = false;
bool interrupted = false;
uint8_t volume = 127;
bool volume_set = false;  // false: adopt the system volume on the first playback
float sys_seen = 0;
double ignore_sys_until = 0;
NSString* last_route;
MPVolumeView* volume_view;
double last_publish = 0;

bool is_output_sink(AVAudioSessionPort p) { return ![p isEqualToString:AVAudioSessionPortBuiltInSpeaker] && ![p isEqualToString:AVAudioSessionPortBuiltInReceiver]; }

AVAudioSessionPortDescription* sink_port() {
    for (AVAudioSessionPortDescription* o in AVAudioSession.sharedInstance.currentRoute.outputs)
        if (is_output_sink(o.portType)) return o;
    return nil;
}

constexpr float kMaxBoost = 4;  // +12 dB; more only flattens the music against the limiter

// The watch's slider is the music's loudness, whatever the system volume is at: above it the
// player attenuates, so raising the phone for another app doesn't make the music louder; below it
// the player boosts. Both scales are taken as squared, like the companion's software volume;
// iOS's real curve is unmeasured.
float gain() {
    float want = volume / 127.f, have = AVAudioSession.sharedInstance.outputVolume;
    if (want <= 0) return 0;
    if (have <= 0) return kMaxBoost;
    float r = want / have;
    return std::min(r * r, kMaxBoost);
}

// Raising the system volume is cleaner than boosting. iOS has no API for it: this is the slider
// inside MPVolumeView, which may do nothing in the background; then the boost makes up for it.
void raise_system_volume(float to) {
    for (UIView* v in volume_view.subviews)
        if (auto* slider = [v isKindOfClass:UISlider.class] ? (UISlider*)v : nil) {
            ignore_sys_until = mono_s() + 1;
            [slider setValue:to animated:NO];
            [slider sendActionsForControlEvents:UIControlEventValueChanged];
        }
}

void save_volume() {
    volume_set = true;
    [NSUserDefaults.standardUserDefaults setInteger:volume forKey:kVolumeKey];
}

// The session must be active for outputVolume to be current.
void apply_volume() {
    sys_seen = AVAudioSession.sharedInstance.outputVolume;
    if (!volume_set && playing) {
        volume = static_cast<uint8_t>(std::lround(sys_seen * 127));
        save_volume();
    }
    float want = volume / 127.f;
    if (playing && sys_seen < want - 0.01f) raise_system_volume(want);
    ap.volume = gain();
}

NSString* route_id() { return AVAudioSession.sharedInstance.currentRoute.outputs.firstObject.UID ?: @""; }

void update_now_playing() {
    MPNowPlayingInfoCenter* c = MPNowPlayingInfoCenter.defaultCenter;
    if (!ap) {
        c.nowPlayingInfo = nil;
        return;
    }
    c.nowPlayingInfo = @{MPMediaItemPropertyTitle: ns(cur.title), MPMediaItemPropertyArtist: ns(cur.artist), MPMediaItemPropertyAlbumTitle: ns(cur.album),
                         MPMediaItemPropertyPlaybackDuration: @(ap.duration), MPNowPlayingInfoPropertyElapsedPlaybackTime: @(ap.currentTime),
                         MPNowPlayingInfoPropertyPlaybackRate: @(playing ? 1.0 : 0.0)};
}

void publish() {
    swp::PlaybackStatus s = status();
    s.up_next.clear();
    for (uint16_t i : order.upcoming(4))
        if (i < queue_tracks.size()) s.up_next.push_back({queue_tracks[i].title, queue_tracks[i].artist});
    st.up_next = s.up_next;
    last_publish = mono_s();
    update_now_playing();
    ble::send(s);
}

void set_state(swp::PlayState s, const std::string& err = "") {
    st.state = static_cast<uint8_t>(s);
    st.error = err;
    publish();
}

void close_track() {
    [ap stop];
    ap = nil;
}

bool open_track(uint32_t start_ms) {
    close_track();
    int idx = order.current();
    if (idx < 0 || static_cast<size_t>(idx) >= queue_tracks.size()) return false;
    cur = queue_tracks[idx];
    if (!cur.available) return false;
    std::string path = cur.path.empty() ? library::path_for(cur.id, cur.suffix) : cur.path;
    NSError* err;
    ap = [[SWTrack alloc] initWithPath:ns(path) error:&err];
    if (!ap) {
        NSLog(@"cannot play %s: %@", path.c_str(), err);
        return false;
    }
    apply_volume();
    if (start_ms) ap.currentTime = start_ms / 1000.0;
    st.playlist_id = playlist;
    st.index = static_cast<uint16_t>(idx);
    st.track_id = cur.id;
    st.title = cur.title;
    st.artist = cur.artist;
    st.album = cur.album;
    if (playing && ![ap play]) {
        close_track();
        return false;
    }
    return true;
}

bool advance(bool user) {
    if (!user && playlist == memos::kPlaylistId) return false;  // a memo plays on its own, then stops
    for (uint16_t tries = 0; tries <= order.size(); ++tries) {
        if (!order.next(user)) return false;
        if (open_track(0)) {
            if (user || order.repeat() != ccore::Repeat::One) order.reshuffle_upcoming(arc4random());
            return true;
        }
        user = true;  // skip unavailable tracks even in repeat-one
    }
    return false;
}

void stop_playback() {
    playing = false;
    close_track();
    apply_session();
    set_state(swp::PlayState::Stopped);
}

void handle(const swp::PlaybackCommand& c) {
    switch (static_cast<PlaybackCmd>(c.cmd)) {
        case PlaybackCmd::PlayPlaylist: {
            if (memos::active()) return;
            playlist = c.str.empty() ? library::kAllSongsId : c.str;
            queue_tracks = playlist == memos::kPlaylistId ? memos::tracks() : library::tracks_of(playlist);
            uint16_t n = static_cast<uint16_t>(std::min<size_t>(queue_tracks.size(), UINT16_MAX));
            if (n == 0) return set_state(swp::PlayState::Error, "playlist not synced");
            // arg UINT32_MAX: start from a random track (Shuffle buttons).
            uint16_t start = c.arg == UINT32_MAX ? static_cast<uint16_t>(arc4random_uniform(n)) : static_cast<uint16_t>(std::min<uint32_t>(c.arg, n - 1));
            order.load(n, start, order.shuffle(), arc4random());
            playing = true;
            apply_session();
            if (!open_track(0) && !advance(true)) {
                playing = false;
                apply_session();
                return set_state(swp::PlayState::Error, "no downloaded tracks");
            }
            set_state(swp::PlayState::Playing);
            break;
        }
        case PlaybackCmd::Toggle:
            return handle({static_cast<uint8_t>(playing ? PlaybackCmd::Pause : PlaybackCmd::Resume), 0, ""});
        case PlaybackCmd::Pause:
            if (!playing) return;
            playing = false;
            [ap pause];
            set_state(swp::PlayState::Paused);
            break;
        case PlaybackCmd::Resume:
            if (memos::active()) return;
            if (!ap) {
                if (!playlist.empty()) return handle({static_cast<uint8_t>(PlaybackCmd::PlayPlaylist), st.index, playlist});
                // Nothing loaded: shuffle All Songs from a random track.
                if (library::downloaded() == 0) return set_state(swp::PlayState::Error, "no songs downloaded yet");
                order.set_shuffle(true, arc4random());
                order.set_repeat(ccore::Repeat::All);
                return handle({static_cast<uint8_t>(PlaybackCmd::PlayPlaylist), UINT32_MAX, library::kAllSongsId});
            }
            playing = true;
            apply_session();
            apply_volume();
            if (![ap play]) {
                playing = false;
                return set_state(swp::PlayState::Error, "audio output failed");
            }
            set_state(swp::PlayState::Playing);
            break;
        case PlaybackCmd::SkipTo:
            if (c.arg < 1 || c.arg > 4 || order.empty()) break;
            for (uint32_t k = 1; k < c.arg; ++k) order.next(true);
            [[fallthrough]];
        case PlaybackCmd::Next:
            if (!advance(true)) return stop_playback();
            publish();
            break;
        case PlaybackCmd::Previous: {
            bool restart = ap && ap.currentTime > 3;
            order.prev(restart);
            if (!open_track(0)) {
                if (!advance(true)) return stop_playback();
            } else if (!restart) {
                order.reshuffle_upcoming(arc4random());
            }
            publish();
            break;
        }
        case PlaybackCmd::Seek:
            if (ap) {
                ap.currentTime = std::min(c.arg / 1000.0, ap.duration);
                publish();
            }
            break;
        case PlaybackCmd::SetShuffle:
            order.set_shuffle(c.arg != 0, arc4random());
            publish();
            break;
        case PlaybackCmd::SetRepeat:
            order.set_repeat(static_cast<ccore::Repeat>(std::min<uint32_t>(c.arg, 2)));
            publish();
            break;
        case PlaybackCmd::Stop:
            stop_playback();
            break;
        case PlaybackCmd::SetVolume:
            volume = static_cast<uint8_t>(std::min<uint32_t>(c.arg, 127));
            save_volume();
            apply_volume();
            publish();
            break;
        case PlaybackCmd::AlertTone: {
            if (!sink_port()) break;  // only ever on headphones, like the companion
            unsigned repeats = std::clamp<unsigned>(c.arg ? c.arg : 3, 1, 10);
            // 880 Hz beeps, 400 ms on / 200 ms off.
            constexpr int kRate = 22050;
            std::vector<int16_t> pcm(repeats * kRate * 6 / 10);
            for (size_t i = 0; i < pcm.size(); ++i)
                pcm[i] = (i % (kRate * 6 / 10)) < static_cast<size_t>(kRate * 4 / 10) ? static_cast<int16_t>(11000 * std::sin(2 * M_PI * 880.0 * i / kRate)) : 0;
            alert_player = [[AVAudioPlayer alloc] initWithData:wav_data(pcm, kRate) error:nil];
            alert_player.delegate = delegate;
            apply_session();
            [alert_player play];
            break;
        }
    }
}

}  // namespace

NSData* wav_data(const std::vector<int16_t>& pcm, int rate) {
    uint32_t data = static_cast<uint32_t>(pcm.size() * 2);
    NSMutableData* d = [NSMutableData dataWithCapacity:44 + data];
    auto u32 = [&](uint32_t v) { [d appendBytes:&v length:4]; };
    auto u16 = [&](uint16_t v) { [d appendBytes:&v length:2]; };
    auto tag = [&](const char* s) { [d appendBytes:s length:4]; };
    tag("RIFF"); u32(36 + data); tag("WAVE"); tag("fmt "); u32(16); u16(1); u16(1);
    u32(rate); u32(rate * 2); u16(2); u16(16); tag("data"); u32(data);
    [d appendBytes:pcm.data() length:data];
    return d;
}

// A volume button (phone or headphones) during playback moves the music by the same step. Other
// changes, while paused or from a new route, only adjust the attenuation.
void on_system_volume() {
    float now = AVAudioSession.sharedInstance.outputVolume, delta = now - sys_seen;
    sys_seen = now;
    NSString* route = route_id();
    bool same_route = [route isEqualToString:last_route];
    last_route = route;
    if (playing && same_route && delta != 0 && mono_s() >= ignore_sys_until) {
        volume = static_cast<uint8_t>(std::clamp<long>(volume + std::lround(delta * 127), 0, 127));
        save_volume();
        publish();
    }
    ap.volume = gain();
}

void attach_volume_view(UIView* parent) {
    if (volume_view) return;
    volume_view = [[MPVolumeView alloc] initWithFrame:CGRectMake(-1000, -1000, 1, 1)];  // off screen also hides the system's volume overlay
    [parent addSubview:volume_view];
}

void on_finished(AVAudioPlayer* p) {
    if (p != alert_player) return;
    alert_player = nil;
    apply_session();
}

void track_finished(SWTrack* t) {
    if (t != ap) return;
    if (!advance(false)) return stop_playback();
    publish();
}

void apply_session() {
    AVAudioSession* s = AVAudioSession.sharedInstance;
    NSError* err = nil;
    if (memos::active()) return;  // the recorder set up its own session
    if (playing || ap) {
        [s setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:0 error:&err];
        [s setActive:YES error:&err];
    } else if (alert_player || silence) {
        [s setCategory:AVAudioSessionCategoryPlayback mode:AVAudioSessionModeDefault options:AVAudioSessionCategoryOptionMixWithOthers error:&err];
        [s setActive:YES error:&err];
    } else {
        [s setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&err];
    }
    if (err) NSLog(@"audio session: %@", err);
    if (silence) [silence play];
}

void command(const swp::PlaybackCommand& c) { handle(c); }

swp::PlaybackStatus status() {
    swp::PlaybackStatus s = st;
    s.position_ms = ap ? static_cast<uint32_t>(ap.currentTime * 1000) : 0;
    s.duration_ms = ap ? static_cast<uint32_t>(ap.duration * 1000) : 0;
    s.shuffle = order.shuffle();
    s.repeat = static_cast<uint8_t>(order.repeat());
    s.volume = volume;
    s.sink_connected = sink_port() != nil;
    return s;
}

bool active() { return playing; }

void pause_for_memo() {
    if (playing) handle({static_cast<uint8_t>(PlaybackCmd::Pause), 0, ""});
}

bool keep_alive() { return silence != nil; }

void set_keep_alive(bool on) {
    [NSUserDefaults.standardUserDefaults setBool:on forKey:kKeepAliveKey];
    if (on && !silence) {
        silence = [[AVAudioPlayer alloc] initWithData:wav_data(std::vector<int16_t>(8000), 8000) error:nil];
        silence.numberOfLoops = -1;
        silence.volume = 0;
    } else if (!on) {
        [silence stop];
        silence = nil;
    }
    apply_session();
}

swp::AudioStatus audio_status() {
    swp::AudioStatus a;
    AVAudioSessionPortDescription* p = sink_port();
    a.state = static_cast<uint8_t>(!p ? swp::SinkState::Idle : playing ? swp::SinkState::Streaming : swp::SinkState::Connected);
    if (p) a.name = str(p.portName);
    a.remembered = p != nil;
    return a;
}

swp::AudioDevices audio_devices() {
    swp::AudioDevices d;
    if (AVAudioSessionPortDescription* p = sink_port()) {
        swp::AudioDevice x;
        x.name = str(p.portName);
        x.remembered = 1;
        d.devices.push_back(std::move(x));
    }
    return d;
}

void push_audio_status() { ble::send(audio_status()); }

void start() {
    delegate = [SWPlayerDelegate new];
    if ([NSUserDefaults.standardUserDefaults objectForKey:kVolumeKey]) {
        volume = static_cast<uint8_t>([NSUserDefaults.standardUserDefaults integerForKey:kVolumeKey]);
        volume_set = true;
    }
    last_route = route_id();
    [AVAudioSession.sharedInstance addObserver:delegate forKeyPath:@"outputVolume" options:0 context:nullptr];
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    NSOperationQueue* main = NSOperationQueue.mainQueue;
    [nc addObserverForName:AVAudioSessionInterruptionNotification object:nil queue:main usingBlock:^(NSNotification* n) {
        auto type = static_cast<AVAudioSessionInterruptionType>([n.userInfo[AVAudioSessionInterruptionTypeKey] unsignedIntegerValue]);
        if (type == AVAudioSessionInterruptionTypeBegan) {
            if (memos::active()) memos::command({static_cast<uint8_t>(swp::MemoOp::Stop), ""});
            if (playing) {
                interrupted = true;
                handle({static_cast<uint8_t>(PlaybackCmd::Pause), 0, ""});
            }
        } else {
            apply_session();  // restarts the keep-alive
            auto opts = [n.userInfo[AVAudioSessionInterruptionOptionKey] unsignedIntegerValue];
            if (interrupted && (opts & AVAudioSessionInterruptionOptionShouldResume)) handle({static_cast<uint8_t>(PlaybackCmd::Resume), 0, ""});
            interrupted = false;
        }
    }];
    [nc addObserverForName:AVAudioSessionRouteChangeNotification object:nil queue:main usingBlock:^(NSNotification* n) {
        auto reason = static_cast<AVAudioSessionRouteChangeReason>([n.userInfo[AVAudioSessionRouteChangeReasonKey] unsignedIntegerValue]);
        // Headphones gone: pause rather than carry on through the speaker.
        if (reason == AVAudioSessionRouteChangeReasonOldDeviceUnavailable && playing) handle({static_cast<uint8_t>(PlaybackCmd::Pause), 0, ""});
        last_route = route_id();
        ignore_sys_until = mono_s() + 1;  // each route has its own system volume
        apply_volume();
        push_audio_status();
        publish();
    }];

    // Headphone and lock screen controls. A press while a memo records stops the recording instead.
    MPRemoteCommandCenter* rc = MPRemoteCommandCenter.sharedCommandCenter;
    auto bind = [](MPRemoteCommand* cmd, PlaybackCmd pc) {
        [cmd addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent*) {
            if (!memos::take_button()) handle({static_cast<uint8_t>(pc), 0, ""});
            return MPRemoteCommandHandlerStatusSuccess;
        }];
    };
    // Headphones may send play or pause depending on what they think is happening: both toggle.
    bind(rc.playCommand, PlaybackCmd::Toggle);
    bind(rc.pauseCommand, PlaybackCmd::Toggle);
    bind(rc.togglePlayPauseCommand, PlaybackCmd::Toggle);
    bind(rc.nextTrackCommand, PlaybackCmd::Next);
    bind(rc.previousTrackCommand, PlaybackCmd::Previous);
    [rc.changePlaybackPositionCommand addTargetWithHandler:^MPRemoteCommandHandlerStatus(MPRemoteCommandEvent* e) {
        auto* ev = (MPChangePlaybackPositionCommandEvent*)e;
        handle({static_cast<uint8_t>(PlaybackCmd::Seek), static_cast<uint32_t>(ev.positionTime * 1000), ""});
        return MPRemoteCommandHandlerStatusSuccess;
    }];

    [NSTimer scheduledTimerWithTimeInterval:1 repeats:YES block:^(NSTimer*) {
        (void)ap.currentTime;  // keeps the position fresh for restarting after a route change
        if (playing && mono_s() - last_publish > 5) publish();
    }];
    // On unless switched off: iOS would otherwise suspend the app and drop the watch.
    id keep = [NSUserDefaults.standardUserDefaults objectForKey:kKeepAliveKey];
    if (!keep || [keep boolValue]) set_keep_alive(true);
}

}  // namespace player

@implementation SWTrack {
    AVAudioFile* _file;
    AVAudioEngine* _engine;
    AVAudioPlayerNode* _node;
    AVAudioUnitEffect* _limiter;
    id _config_observer;
    double _rate, _held;
    AVAudioFramePosition _base;
    uint64_t _gen;  // bumped when a scheduled segment is dropped: its completion is then stale
    bool _wants_play;
}

- (instancetype)initWithPath:(NSString*)path error:(NSError**)err {
    if (!(self = [super init])) return nil;
    _file = [[AVAudioFile alloc] initForReading:[NSURL fileURLWithPath:path] error:err];
    if (!_file || _file.length <= 0) return nil;
    AVAudioFormat* fmt = _file.processingFormat;
    _rate = fmt.sampleRate;
    _duration = _file.length / _rate;
    _engine = [AVAudioEngine new];
    _node = [AVAudioPlayerNode new];
    [_engine attachNode:_node];
    AudioComponentDescription d = {kAudioUnitType_Effect, kAudioUnitSubType_PeakLimiter, kAudioUnitManufacturer_Apple, 0, 0};
    @try {  // AVAudioEngine throws on a format it can't connect
        _limiter = [[AVAudioUnitEffect alloc] initWithAudioComponentDescription:d];
        [_engine attachNode:_limiter];
        [_engine connect:_node to:_limiter format:fmt];
        [_engine connect:_limiter to:_engine.mainMixerNode format:fmt];
    } @catch (NSException* e) {
        NSLog(@"no limiter for %@, so no boost: %@", fmt, e);
        if (_limiter) [_engine detachNode:_limiter];
        _limiter = nil;
        [_engine connect:_node to:_engine.mainMixerNode format:fmt];
    }
    __weak SWTrack* weak = self;
    // The engine stops when the route or the session changes.
    _config_observer = [NSNotificationCenter.defaultCenter addObserverForName:AVAudioEngineConfigurationChangeNotification object:_engine queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification*) {
        SWTrack* s = weak;
        if (s && s->_wants_play && !s->_engine.running) [s play];
    }];
    return self;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:_config_observer];
}

- (void)setVolume:(float)v {
    _volume = v;
    bool boost = v > 1 && _limiter;
    _engine.mainMixerNode.outputVolume = std::min(v, 1.f);
    if (!_limiter) return;
    _limiter.bypass = !boost;
    AudioUnitSetParameter(_limiter.audioUnit, kLimiterParam_PreGain, kAudioUnitScope_Global, 0, boost ? 20 * std::log10(v) : 0, 0);
}

- (double)currentTime {
    AVAudioTime* now = _wants_play && _engine.running ? _node.lastRenderTime : nil;
    AVAudioTime* t = now ? [_node playerTimeForNodeTime:now] : nil;
    if (t && t.sampleTime >= 0) _held = std::min((_base + t.sampleTime) / _rate, _duration);
    return _held;
}

- (void)setCurrentTime:(double)t {
    _held = std::clamp(t, 0.0, _duration);
    if (_wants_play) [self play];
}

// Always schedules from the held position: resume, seek and restart are all the same thing.
- (BOOL)play {
    ++_gen;
    [_node stop];
    NSError* err;
    if (!_engine.running && ![_engine startAndReturnError:&err]) {
        NSLog(@"audio engine: %@", err);
        _wants_play = false;
        return NO;
    }
    _wants_play = true;
    _base = std::min<AVAudioFramePosition>(std::llround(_held * _rate), _file.length);
    uint64_t gen = _gen;
    __weak SWTrack* weak = self;
    void (^finished)(void) = ^{
        dispatch_async(dispatch_get_main_queue(), ^{
            SWTrack* s = weak;
            if (s && s->_gen == gen) player::track_finished(s);
        });
    };
    if (_base >= _file.length) {
        finished();
        return YES;
    }
    [_node scheduleSegment:_file startingFrame:_base frameCount:static_cast<AVAudioFrameCount>(_file.length - _base) atTime:nil
        completionCallbackType:AVAudioPlayerNodeCompletionDataPlayedBack
             completionHandler:^(AVAudioPlayerNodeCompletionCallbackType) { finished(); }];
    [_node play];
    return YES;
}

- (void)pause {
    (void)self.currentTime;
    _wants_play = false;
    ++_gen;
    [_node stop];
    [_engine pause];
}

- (void)stop {
    _wants_play = false;
    ++_gen;
    [_node stop];
    [_engine stop];
}
@end

@implementation SWPlayerDelegate
- (void)observeValueForKeyPath:(NSString*)path ofObject:(id)o change:(NSDictionary*)c context:(void*)ctx {
    dispatch_async(dispatch_get_main_queue(), ^{ player::on_system_volume(); });
}
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)p successfully:(BOOL)ok {
    dispatch_async(dispatch_get_main_queue(), ^{ player::on_finished(p); });
}
- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer*)p error:(NSError*)e {
    NSLog(@"decode error: %@", e);
    dispatch_async(dispatch_get_main_queue(), ^{ player::on_finished(p); });
}
@end
