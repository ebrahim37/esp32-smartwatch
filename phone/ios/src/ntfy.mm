// ntfy subscription: a long-lived streaming request, with at-least-once delivery to the watch
// (port of firmware/companion/main/ntfy.cpp).
#include <atomic>

#include "app.hpp"
#include "ccore/backoff.hpp"
#include "ccore/ntfy.hpp"

namespace {

NSString* const kCursor = @"ntfy.cursor";
constexpr const char* kFirstSince = "10m";  // nothing acknowledged yet: recent history only
constexpr double kIdleS = 120;              // ntfy keepalive is ~45 s

ccore::ntfy::AckCursor* cursor;
ccore::ntfy::LineSplitter lines;
ccore::Backoff backoff(5000, 300000, 20);
NSURLSession* sess;
NSURLSessionDataTask* task;
uint32_t last_event = 0;
bool subscribed = false;
uint32_t generation = 0;

void on_line(std::string_view line) {
    id j = [NSJSONSerialization JSONObjectWithData:[NSData dataWithBytes:line.data() length:line.size()] options:0 error:nil];
    if (![j isKindOfClass:NSDictionary.class]) return;
    last_event = static_cast<uint32_t>(time(nullptr));
    std::string id = jstr(j, @"id");
    if (jstr(j, @"event") != "message" || id.empty() || id.size() >= 64 || !cursor->add(id)) return;
    swp::Notification n;
    n.id = ccore::ntfy::message_key(id);
    double t = jnum(j, @"time");
    n.timestamp = t > 0 ? static_cast<uint32_t>(t) : static_cast<uint32_t>(time(nullptr));
    int p = static_cast<int>(jnum(j, @"priority"));
    n.priority = static_cast<uint8_t>(p >= 1 && p <= 5 ? p : 3);
    n.topic = jstr(j, @"topic").substr(0, 64);
    n.title = jstr(j, @"title").substr(0, 128);
    n.body = jstr(j, @"message").substr(0, 1024);
    cloud::ingest_notification(n);
}

void subscribe();

void schedule_retry() {
    uint32_t ms = backoff.next(arc4random());
    uint32_t gen = generation;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, static_cast<int64_t>(ms) * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (gen == generation && !task) subscribe();
    });
}

void restart() {
    ++generation;
    [task cancel];
    task = nil;
    subscribed = false;
    subscribe();
}

}  // namespace

@interface SWNtfy : NSObject <NSURLSessionDataDelegate>
@end

@implementation SWNtfy

- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t willPerformHTTPRedirection:(NSHTTPURLResponse*)r newRequest:(NSURLRequest*)req completionHandler:(void (^)(NSURLRequest*))h {
    h(nil);  // the auth header must not follow a redirect
}

- (void)URLSession:(NSURLSession*)s dataTask:(NSURLSessionDataTask*)t didReceiveResponse:(NSURLResponse*)r completionHandler:(void (^)(NSURLSessionResponseDisposition))h {
    if (t != task) return h(NSURLSessionResponseCancel);
    int status = static_cast<int>(((NSHTTPURLResponse*)r).statusCode);
    if (status != 200) {
        NSLog(@"ntfy: subscribe failed: HTTP %d", status);
        return h(NSURLSessionResponseCancel);
    }
    subscribed = true;
    backoff.reset();
    net::mark_internet();
    lines.reset();
    h(NSURLSessionResponseAllow);
}

- (void)URLSession:(NSURLSession*)s dataTask:(NSURLSessionDataTask*)t didReceiveData:(NSData*)d {
    if (t != task) return;
    net::mark_internet();  // message or keepalive over the TLS stream
    lines.feed(static_cast<const char*>(d.bytes), d.length, on_line);
}

- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t didCompleteWithError:(NSError*)e {
    if (t != task) return;
    task = nil;
    subscribed = false;
    schedule_retry();
}

@end

namespace {

void subscribe() {
    if (task) return;
    std::string base = cfg::get(cfg::kNtfyUrl);
    while (!base.empty() && base.back() == '/') base.pop_back();
    auto topics = ccore::ntfy::split_topics(cfg::get(cfg::kNtfyTopics));
    if (base.rfind("https://", 0) != 0 || topics.empty() || !net::online()) return;  // a config or network change retries
    std::string since = cursor->cursor().empty() ? kFirstSince : cursor->cursor();
    NSURL* u = [NSURL URLWithString:ns(base + "/" + ccore::ntfy::join_topics(topics) + "/json?since=" + since)];
    if (!u) return;
    NSMutableURLRequest* r = [NSMutableURLRequest requestWithURL:u];
    r.cachePolicy = NSURLRequestReloadIgnoringLocalCacheData;
    std::string token = cfg::get(cfg::kNtfyToken);
    if (!token.empty()) [r setValue:ns(token) forHTTPHeaderField:@"auth"];
    task = [sess dataTaskWithRequest:r];
    [task resume];
}

}  // namespace

namespace ntfy {

void start() {
    NSString* c = [NSUserDefaults.standardUserDefaults stringForKey:kCursor];
    cursor = new ccore::ntfy::AckCursor(c ? str(c) : "");
    NSURLSessionConfiguration* conf = NSURLSessionConfiguration.ephemeralSessionConfiguration;
    conf.timeoutIntervalForRequest = kIdleS;  // the stream counts as dead after this long without data
    conf.URLCache = nil;
    static SWNtfy* delegate = [SWNtfy new];
    sess = [NSURLSession sessionWithConfiguration:conf delegate:delegate delegateQueue:NSOperationQueue.mainQueue];
    cfg::add_listener([](const std::string& key) {
        if (key == cfg::kNtfyUrl || key == cfg::kNtfyTopics || key == cfg::kNtfyToken) restart();
    });
    net::add_listener([] {
        if (net::online() && !subscribed) restart();
    });
    subscribe();
}

void on_ack(uint64_t key) {
    if (!cursor) return;
    if (auto adv = cursor->ack(key)) [NSUserDefaults.standardUserDefaults setObject:ns(*adv) forKey:kCursor];
}

uint32_t last_event_utc() { return last_event; }
bool connected() { return subscribed; }

}  // namespace ntfy
