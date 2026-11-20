// Status and settings screen. Plain UIKit, no images.
#import <UIKit/UIKit.h>

#include "app.hpp"

namespace {

const char* sync_text(swp::SyncState s) {
    switch (s) {
        case swp::SyncState::Idle: return "Idle";
        case swp::SyncState::Running: return "Syncing";
        case swp::SyncState::Done: return "Up to date";
        case swp::SyncState::Failed: return "Failed, will retry";
        case swp::SyncState::Paused: return "Waiting for network";
        case swp::SyncState::NotConfigured: return "Not configured";
    }
    return "";
}

std::string gb(uint64_t bytes) {
    char b[32];
    snprintf(b, sizeof b, "%.1f GB", bytes / 1073741824.0);
    return b;
}

enum Section { kStatus, kWatch, kServer, kMusic, kNotifications, kDevice, kSectionCount };

std::vector<const cfg::KeyInfo*> section_keys(NSInteger s) {
    std::vector<const char*> names;
    if (s == kServer) names = {cfg::kServerUrl, cfg::kServerToken};
    if (s == kMusic) names = {cfg::kNdUrl, cfg::kNdUser, cfg::kNdPass, cfg::kNdToken};
    if (s == kNotifications) names = {cfg::kNtfyUrl, cfg::kNtfyToken, cfg::kNtfyTopics};
    if (s == kDevice) names = {cfg::kDeviceName, cfg::kTimezone};
    std::vector<const cfg::KeyInfo*> out;
    for (auto* n : names) out.push_back(cfg::find(n));
    return out;
}

std::vector<std::pair<std::string, std::string>> status_rows() {
    std::vector<std::pair<std::string, std::string>> r;
    r.push_back({"Watch", ble::status_text()});
    if (!ble::last_drop_text().empty()) r.push_back({"Last disconnect", ble::last_drop_text()});
    r.push_back({"Link debug", ble::debug_text()});
    r.push_back({"Location", position::status_text()});
    r.push_back({"Internet", !net::online() ? "Offline" : net::wifi() ? "Wi-Fi" : "Cellular"});
    r.push_back({"Server", cfg::is_set(cfg::kServerUrl) ? (net::server_ok() ? "Reachable" : "No reply yet") : "Not configured"});
    r.push_back({"Notifications", ntfy::connected() ? "Subscribed" : "Not subscribed"});
    std::string sync = sync_text(navidrome::state());
    if (navidrome::total()) sync += " (" + std::to_string(navidrome::done()) + "/" + std::to_string(navidrome::total()) + ")";
    r.push_back({"Music sync", sync});
    if (!navidrome::last_error().empty()) r.push_back({"Sync error", navidrome::last_error()});
    r.push_back({"Songs", std::to_string(library::downloaded()) + ", " + gb(navidrome::used_bytes())});
    r.push_back({"Map cache", gb(cloud::maps_used())});
    r.push_back({"Free space", gb(paths::free_bytes())});
    r.push_back({"Events", diag::text()});
    return r;
}

}  // namespace

@interface SWSettings : UITableViewController
@end

@implementation SWSettings {
    UISwitch* _keepAlive;
}

- (instancetype)init {
    self = [super initWithStyle:UITableViewStyleInsetGrouped];
    self.title = @"SW Companion";
    return self;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    _keepAlive = [UISwitch new];
    [_keepAlive addTarget:self action:@selector(keepAliveChanged) forControlEvents:UIControlEventValueChanged];
    __weak SWSettings* weak = self;
    [NSTimer scheduledTimerWithTimeInterval:2 repeats:YES block:^(NSTimer*) {
        SWSettings* s = weak;
        if (s.viewIfLoaded.window) [s.tableView reloadSections:[NSIndexSet indexSetWithIndex:kStatus] withRowAnimation:UITableViewRowAnimationNone];
    }];
}

- (void)keepAliveChanged { player::set_keep_alive(_keepAlive.isOn); }

- (NSInteger)numberOfSectionsInTableView:(UITableView*)t { return kSectionCount; }

- (NSString*)tableView:(UITableView*)t titleForHeaderInSection:(NSInteger)s {
    switch (s) {
        case kStatus: return @"Status";
        case kWatch: return @"Watch";
        case kServer: return @"VPS helper";
        case kMusic: return @"Navidrome";
        case kNotifications: return @"ntfy";
        case kDevice: return @"Device";
    }
    return nil;
}

- (NSString*)tableView:(UITableView*)t titleForFooterInSection:(NSInteger)s {
    if (s == kWatch) return @"Forgetting here also needs Forget in iOS Bluetooth settings and on the watch. Silent audio helps the app stay running in the background.";
    return nil;
}

- (NSInteger)tableView:(UITableView*)t numberOfRowsInSection:(NSInteger)s {
    if (s == kStatus) return static_cast<NSInteger>(status_rows().size());
    if (s == kWatch) return 3;
    return static_cast<NSInteger>(section_keys(s).size());
}

- (UITableViewCell*)tableView:(UITableView*)t cellForRowAtIndexPath:(NSIndexPath*)p {
    auto* c = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1 reuseIdentifier:nil];
    c.selectionStyle = UITableViewCellSelectionStyleNone;
    c.detailTextLabel.numberOfLines = 2;
    if (p.section == kStatus) {
        auto rows = status_rows();
        c.textLabel.text = ns(rows[p.row].first);
        c.detailTextLabel.text = ns(rows[p.row].second);
        c.detailTextLabel.numberOfLines = 0;
        c.selectionStyle = UITableViewCellSelectionStyleDefault;
    } else if (p.section == kWatch) {
        c.selectionStyle = UITableViewCellSelectionStyleDefault;
        if (p.row == 0) {
            bool paired = ble::has_peer();
            c.textLabel.text = paired ? @"Forget this watch" : @"Look for a watch";
            c.textLabel.textColor = paired ? UIColor.systemRedColor : UIColor.systemBlueColor;
        } else if (p.row == 1) {
            c.textLabel.text = @"Sync music now";
            c.textLabel.textColor = UIColor.systemBlueColor;
        } else {
            c.selectionStyle = UITableViewCellSelectionStyleNone;
            c.textLabel.text = @"Silent audio keep-alive";
            _keepAlive.on = player::keep_alive();
            c.accessoryView = _keepAlive;
        }
    } else {
        const cfg::KeyInfo* k = section_keys(p.section)[p.row];
        c.selectionStyle = UITableViewCellSelectionStyleDefault;
        c.textLabel.text = @(k->label);
        c.detailTextLabel.text = k->secret ? (cfg::is_set(k->key) ? @"Saved" : @"Not set") : (ns(cfg::get(k->key)).length ? ns(cfg::get(k->key)) : @"Not set");
        c.detailTextLabel.lineBreakMode = NSLineBreakByTruncatingMiddle;
        c.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
    }
    return c;
}

- (void)tableView:(UITableView*)t didSelectRowAtIndexPath:(NSIndexPath*)p {
    [t deselectRowAtIndexPath:p animated:YES];
    if (p.section == kWatch) {
        if (p.row == 0) {
            if (ble::has_peer()) ble::forget();
            else ble::connect();
            [t reloadData];
        } else if (p.row == 1) {
            navidrome::trigger();
        }
        return;
    }
    if (p.section == kStatus) {  // the full text, to copy
        auto rows = status_rows();
        NSString* text = ns(rows[p.row].second);
        UIAlertController* a = [UIAlertController alertControllerWithTitle:ns(rows[p.row].first) message:text preferredStyle:UIAlertControllerStyleAlert];
        [a addAction:[UIAlertAction actionWithTitle:@"Copy" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) { UIPasteboard.generalPasteboard.string = text; }]];
        [a addAction:[UIAlertAction actionWithTitle:@"Close" style:UIAlertActionStyleCancel handler:nil]];
        [self presentViewController:a animated:YES completion:nil];
        return;
    }
    const cfg::KeyInfo* k = section_keys(p.section)[p.row];
    std::string key = k->key;
    bool secret = k->secret;
    UIAlertController* a = [UIAlertController alertControllerWithTitle:@(k->label) message:secret ? @"Saved values are never shown. Leave empty to clear." : nil preferredStyle:UIAlertControllerStyleAlert];
    [a addTextFieldWithConfigurationHandler:^(UITextField* f) {
        f.secureTextEntry = secret;
        f.text = secret ? @"" : ns(cfg::get(key.c_str()));
        f.autocapitalizationType = UITextAutocapitalizationTypeNone;
        f.autocorrectionType = UITextAutocorrectionTypeNo;
        f.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [a addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak UIAlertController* weak = a;
    [a addAction:[UIAlertAction actionWithTitle:@"Save" style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
        std::string v = str([weak.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]);
        if (!cfg::set(key, v)) {
            UIAlertController* e = [UIAlertController alertControllerWithTitle:@"Not saved" message:@"That value isn't valid (URLs must be https; Navidrome may also be http on a private LAN address)." preferredStyle:UIAlertControllerStyleAlert];
            [e addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
            [self presentViewController:e animated:YES completion:nil];
        }
        [self.tableView reloadData];
    }]];
    [self presentViewController:a animated:YES completion:nil];
}

@end

namespace ui {

id make_root() { return [[UINavigationController alloc] initWithRootViewController:[SWSettings new]]; }

}  // namespace ui
