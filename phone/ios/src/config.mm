// Settings: plain values in NSUserDefaults, secrets in a file-protected, non-backed-up file in the
// app's data.
#include <map>
#include <mutex>

#include "app.hpp"
#include "ccore/ntfy.hpp"
#include "ccore/subsonic.hpp"

namespace cfg {
namespace {

const std::vector<KeyInfo> kKeys = {
    {kServerUrl, false, 128, "VPS helper URL"},
    {kServerToken, true, 128, "VPS helper token"},
    {kNdUrl, false, 128, "Navidrome URL"},
    {kNdUser, false, 64, "Navidrome user"},
    {kNdPass, true, 128, "Navidrome password"},
    {kNdToken, true, 128, "Navidrome auth header"},
    {kNtfyUrl, false, 128, "ntfy URL"},
    {kNtfyToken, true, 128, "ntfy auth header"},
    {kNtfyTopics, false, 512, "ntfy topics"},
    {kTimezone, false, 64, "Time zone (POSIX TZ)"},
    {kDeviceName, false, 24, "Device name"},
};

std::mutex mu;
std::map<std::string, std::string> secrets;  // guarded by mu
bool loaded = false;
std::vector<std::function<void(const std::string&)>> listeners;

NSString* defaults_key(const char* key) { return [@"cfg." stringByAppendingString:@(key)]; }

NSString* secrets_file() {
    NSString* dir = NSSearchPathForDirectoriesInDomains(NSApplicationSupportDirectory, NSUserDomainMask, YES).firstObject;
    [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    return [dir stringByAppendingPathComponent:@"secrets.plist"];
}

NSDictionary* read_secrets_file() { return [NSDictionary dictionaryWithContentsOfFile:secrets_file()] ?: @{}; }

bool write_secrets_file(NSDictionary* d) {
    NSError* err;
    NSData* data = [NSPropertyListSerialization dataWithPropertyList:d format:NSPropertyListBinaryFormat_v1_0 options:0 error:&err];
    NSString* p = secrets_file();
    // Readable after the first unlock: the app works in the background on a locked phone.
    if (!data || ![data writeToFile:p options:NSDataWritingAtomic | NSDataWritingFileProtectionCompleteUntilFirstUserAuthentication error:&err]) return false;
    NSURL* u = [NSURL fileURLWithPath:p];
    [u setResourceValue:@YES forKey:NSURLIsExcludedFromBackupKey error:nil];
    return true;
}

// Caller holds mu.
void load_secrets() {
    if (loaded) return;
    loaded = true;
    NSDictionary* file = read_secrets_file();
    for (auto& k : kKeys)
        if (NSString* v = k.secret ? file[@(k.key)] : nil) secrets[k.key] = str(v);
}

// Caller holds mu.
bool store_secret(const char* key, const std::string& value) {
    NSMutableDictionary* file = [read_secrets_file() mutableCopy];
    if (value.empty()) [file removeObjectForKey:@(key)];
    else file[@(key)] = ns(value);
    return write_secrets_file(file);
}

bool valid_value(const KeyInfo& k, const std::string& v) {
    if (v.size() > k.max_len) return false;
    for (unsigned char c : v)
        if (c < 0x20 && c != '\t') return false;
    std::string key = k.key;
    bool lan_ok = key == kNdUrl && (ccore::lan_http_url(v) || ccore::tailnet_http_url(v));
    if ((key == kServerUrl || key == kNdUrl || key == kNtfyUrl) && !v.empty() && v.rfind("https://", 0) != 0 && !lan_ok) return false;  // TLS only
    if (key == kNtfyTopics && !ccore::ntfy::valid_topic_list(v)) return false;
    return true;
}

}  // namespace

const std::vector<KeyInfo>& keys() { return kKeys; }

const KeyInfo* find(const std::string& key) {
    for (auto& k : kKeys)
        if (key == k.key) return &k;
    return nullptr;
}

std::string get(const char* key, const std::string& def) {
    const KeyInfo* k = find(key);
    if (k && k->secret) {
        std::lock_guard<std::mutex> g(mu);
        load_secrets();
        auto it = secrets.find(key);
        return it == secrets.end() ? def : it->second;
    }
    NSString* v = [NSUserDefaults.standardUserDefaults stringForKey:defaults_key(key)];
    return v.length ? str(v) : def;
}

bool is_set(const char* key) { return !get(key).empty(); }

bool set(const std::string& key, const std::string& value) {
    const KeyInfo* k = find(key);
    if (!k || !valid_value(*k, value)) return false;
    if (k->secret) {
        std::lock_guard<std::mutex> g(mu);
        load_secrets();
        if (!store_secret(k->key, value)) return false;
        if (value.empty()) secrets.erase(key);
        else secrets[key] = value;
    } else if (value.empty()) {
        [NSUserDefaults.standardUserDefaults removeObjectForKey:defaults_key(k->key)];
    } else {
        [NSUserDefaults.standardUserDefaults setObject:ns(value) forKey:defaults_key(k->key)];
    }
    for (auto& l : listeners) l(key);
    return true;
}

void add_listener(std::function<void(const std::string&)> l) { listeners.push_back(std::move(l)); }

}  // namespace cfg
