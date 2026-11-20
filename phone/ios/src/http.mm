// NSURLSession wrappers and connectivity tracking. No authentication-challenge handler is
// installed, so the system's TLS verification always applies.
#import <Network/Network.h>

#include <atomic>
#include <unordered_map>

#include "app.hpp"

namespace {

struct TaskState {
    std::function<void(http::Result)> done;
    http::Result result;
    size_t max = 0;
    std::string dest;
    bool too_large = false;
    bool moved = false;
};
std::unordered_map<NSUInteger, TaskState> tasks;

bool https_url(NSURL* u) { return [u.scheme.lowercaseString isEqualToString:@"https"]; }

}  // namespace

@interface SWHttp : NSObject <NSURLSessionDataDelegate, NSURLSessionDownloadDelegate>
@end

@implementation SWHttp

- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t willPerformHTTPRedirection:(NSHTTPURLResponse*)r newRequest:(NSURLRequest*)req completionHandler:(void (^)(NSURLRequest*))h {
    NSURL* from = t.originalRequest.URL;
    bool same_host = [req.URL.host isEqualToString:from.host];
    NSDictionary* headers = t.originalRequest.allHTTPHeaderFields;
    bool has_secret = headers[@"Authorization"] || headers[@"auth"];
    bool ok = https_url(req.URL) ? (!https_url(from) || same_host || !has_secret) : (!https_url(from) && same_host);
    h(ok ? req : nil);  // nil: the redirect response itself is the result
}

- (void)URLSession:(NSURLSession*)s dataTask:(NSURLSessionDataTask*)t didReceiveData:(NSData*)data {
    auto it = tasks.find(t.taskIdentifier);
    if (it == tasks.end()) return;
    auto& st = it->second;
    if (st.result.body.size() + data.length > st.max) {
        st.too_large = true;
        [t cancel];
        return;
    }
    auto* p = static_cast<const uint8_t*>(data.bytes);
    st.result.body.insert(st.result.body.end(), p, p + data.length);
}

- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t didWriteData:(int64_t)n totalBytesWritten:(int64_t)total totalBytesExpectedToWrite:(int64_t)expected {
    auto it = tasks.find(t.taskIdentifier);
    if (it != tasks.end() && static_cast<uint64_t>(total) > it->second.max) {
        it->second.too_large = true;
        [t cancel];
    }
}

- (void)URLSession:(NSURLSession*)s downloadTask:(NSURLSessionDownloadTask*)t didFinishDownloadingToURL:(NSURL*)loc {
    auto it = tasks.find(t.taskIdentifier);
    if (it == tasks.end()) return;
    auto* r = (NSHTTPURLResponse*)t.response;
    if (![r isKindOfClass:NSHTTPURLResponse.class] || r.statusCode < 200 || r.statusCode >= 300) return;
    NSFileManager* fm = NSFileManager.defaultManager;
    NSString* dest = ns(it->second.dest);
    [fm createDirectoryAtPath:dest.stringByDeletingLastPathComponent withIntermediateDirectories:YES attributes:nil error:nil];
    [fm removeItemAtPath:dest error:nil];
    it->second.moved = [fm moveItemAtURL:loc toURL:[NSURL fileURLWithPath:dest] error:nil];  // must happen before returning
}

- (void)URLSession:(NSURLSession*)s task:(NSURLSessionTask*)t didCompleteWithError:(NSError*)err {
    auto it = tasks.find(t.taskIdentifier);
    if (it == tasks.end()) return;
    TaskState st = std::move(it->second);
    tasks.erase(it);
    auto* r = (NSHTTPURLResponse*)t.response;
    if ([r isKindOfClass:NSHTTPURLResponse.class]) {
        st.result.status = static_cast<int>(r.statusCode);
        st.result.mime = str(r.MIMEType ?: @"");
        if (https_url(r.URL)) net::mark_internet();
    }
    if (err || st.too_large) {
        st.result.status = 0;
        st.result.body.clear();
        st.result.error = st.too_large ? "response too large" : str(err.localizedDescription ?: @"failed");
    } else if (!st.dest.empty() && st.result.ok() && !st.moved) {
        st.result.status = 0;
        st.result.error = "could not store the file";
    }
    if (st.done) st.done(std::move(st.result));
}

@end

namespace http {
namespace {

NSURLSession* session() {
    static NSURLSession* s;
    static SWHttp* delegate;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        delegate = [SWHttp new];
        NSURLSessionConfiguration* c = NSURLSessionConfiguration.ephemeralSessionConfiguration;
        c.URLCache = nil;
        c.HTTPCookieStorage = nil;
        c.HTTPMaximumConnectionsPerHost = 6;
        s = [NSURLSession sessionWithConfiguration:c delegate:delegate delegateQueue:NSOperationQueue.mainQueue];
    });
    return s;
}

NSMutableURLRequest* make_request(const std::string& url, const Options& o) {
    NSURL* u = [NSURL URLWithString:ns(url)];
    if (!u) return nil;
    NSMutableURLRequest* r = [NSMutableURLRequest requestWithURL:u cachePolicy:NSURLRequestReloadIgnoringLocalCacheData timeoutInterval:o.timeout_s];
    r.HTTPMethod = ns(o.method);
    r.HTTPShouldHandleCookies = NO;
    for (auto& [k, v] : o.headers) [r setValue:ns(v) forHTTPHeaderField:ns(k)];
    if (!o.body.empty()) r.HTTPBody = [NSData dataWithBytes:o.body.data() length:o.body.size()];
    return r;
}

void start(NSURLSessionTask* t, size_t max, std::string dest, std::function<void(Result)> done) {
    TaskState st;
    st.done = std::move(done);
    st.max = max;
    st.dest = std::move(dest);
    tasks[t.taskIdentifier] = std::move(st);
    [t resume];
}

void fail(std::function<void(Result)>& done) {
    Result r;
    r.error = "bad url";
    dispatch_async(dispatch_get_main_queue(), ^{ done(r); });
}

Result wait_for(std::function<void(std::function<void(Result)>)> run) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    auto out = std::make_shared<Result>();
    dispatch_async(dispatch_get_main_queue(), ^{
        run([out, sem](Result r) {
            *out = std::move(r);
            dispatch_semaphore_signal(sem);
        });
    });
    dispatch_semaphore_wait(sem, DISPATCH_TIME_FOREVER);
    return std::move(*out);
}

}  // namespace

void request(const std::string& url, Options o, std::function<void(Result)> done) {
    NSMutableURLRequest* r = make_request(url, o);
    if (!r) return fail(done);
    start([session() dataTaskWithRequest:r], o.max_response, "", std::move(done));
}

void download(const std::string& url, const std::string& dest, Options o, std::function<void(Result)> done) {
    NSMutableURLRequest* r = make_request(url, o);
    if (!r) return fail(done);
    start([session() downloadTaskWithRequest:r], o.max_response, dest, std::move(done));
}

Result request_sync(const std::string& url, Options o) {
    return wait_for([&](std::function<void(Result)> d) { request(url, o, std::move(d)); });
}

Result download_sync(const std::string& url, const std::string& dest, Options o) {
    return wait_for([&](std::function<void(Result)> d) { download(url, dest, o, std::move(d)); });
}

}  // namespace http

namespace net {
namespace {

bool is_online = false, is_wifi = false;
std::atomic<bool> wifi_now{false}, online_now{false}, srv_ok{false};
std::atomic<double> internet_at{-1};
std::vector<std::function<void()>> listeners;
nw_path_monitor_t monitor;

}  // namespace

void start() {
    monitor = nw_path_monitor_create();
    nw_path_monitor_set_queue(monitor, dispatch_get_main_queue());
    nw_path_monitor_set_update_handler(monitor, ^(nw_path_t path) {
        bool on = nw_path_get_status(path) == nw_path_status_satisfied;
        bool wf = on && nw_path_uses_interface_type(path, nw_interface_type_wifi);
        bool changed = on != is_online || wf != is_wifi;
        is_online = on;
        is_wifi = wf;
        online_now.store(on);
        wifi_now.store(wf);
        if (changed)
            for (auto& l : listeners) l();
    });
    nw_path_monitor_start(monitor);
}

bool online() { return online_now.load(); }
bool wifi() { return wifi_now.load(); }
void add_listener(std::function<void()> l) { listeners.push_back(std::move(l)); }
void mark_internet() { internet_at.store(mono_s()); }

uint32_t internet_age_s() {
    double t = internet_at.load();
    return t < 0 ? UINT32_MAX : static_cast<uint32_t>(mono_s() - t);
}

void set_server_ok(bool ok) { srv_ok.store(ok); }
bool server_ok() { return srv_ok.load(); }

swp::WifiStatus status() {
    swp::WifiStatus s;
    s.state = static_cast<uint8_t>(online() ? swp::NetState::Connected : swp::NetState::Searching);
    s.provider = static_cast<uint8_t>(online() ? swp::NetProvider::Phone : swp::NetProvider::None);
    s.password_set = 0;
    s.internet_ok = server_ok();
    return s;
}

}  // namespace net
