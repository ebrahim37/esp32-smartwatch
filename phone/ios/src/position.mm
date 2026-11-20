// GPS for the watch. Background location updates also keep the app running in the background.
#import <CoreLocation/CoreLocation.h>

#include <cmath>

#include "app.hpp"

namespace {
CLLocationManager* lm;
bool have = false;
swp::Position pos;
double last_sent = 0;
std::string status = "Waiting for permission";
constexpr double kMinSendS = 2;
}  // namespace

@interface SWLocation : NSObject <CLLocationManagerDelegate>
@end

@implementation SWLocation

- (void)locationManagerDidChangeAuthorization:(CLLocationManager*)m {
    diag::note("location permission " + std::to_string(static_cast<int>(m.authorizationStatus)) + " (3 Always, 4 While Using)");
    switch (m.authorizationStatus) {
        case kCLAuthorizationStatusNotDetermined:
            [m requestWhenInUseAuthorization];
            break;
        case kCLAuthorizationStatusDenied:
        case kCLAuthorizationStatusRestricted:
            status = "Location denied in iOS settings";
            break;
        case kCLAuthorizationStatusAuthorizedWhenInUse:
            // Asked once: iOS may offer the upgrade to "Always", which survives a restart.
            if (![NSUserDefaults.standardUserDefaults boolForKey:@"loc.asked_always"]) {
                [NSUserDefaults.standardUserDefaults setBool:YES forKey:@"loc.asked_always"];
                [m requestAlwaysAuthorization];
            }
            [m startUpdatingLocation];
            break;
        default:
            [m startUpdatingLocation];
    }
}

- (void)locationManager:(CLLocationManager*)m didUpdateLocations:(NSArray<CLLocation*>*)locs {
    CLLocation* l = locs.lastObject;
    if (!l || l.horizontalAccuracy < 0) return;
    bool first = !have;
    pos.lat_e7 = static_cast<int32_t>(std::lround(l.coordinate.latitude * 1e7));
    pos.lon_e7 = static_cast<int32_t>(std::lround(l.coordinate.longitude * 1e7));
    pos.accuracy_m = static_cast<uint16_t>(std::min(l.horizontalAccuracy, 65535.0));
    pos.source = static_cast<uint8_t>(swp::PositionSource::Phone);
    pos.fix_time = static_cast<uint32_t>(l.timestamp.timeIntervalSince1970);
    have = true;
    status = "±" + std::to_string(pos.accuracy_m) + " m";
    if (dispatch::watch_ready() && (first || mono_s() - last_sent >= kMinSendS)) {
        last_sent = mono_s();
        ble::send(pos);
    }
}

- (void)locationManager:(CLLocationManager*)m didFailWithError:(NSError*)e {
    NSLog(@"location: %@", e);
    if (e.code == kCLErrorDenied) status = "Location denied in iOS settings";
}

@end

namespace position {

void start() {
    static SWLocation* delegate = [SWLocation new];
    lm = [CLLocationManager new];
    lm.delegate = delegate;
    lm.desiredAccuracy = kCLLocationAccuracyBest;
    lm.distanceFilter = 5;
    lm.pausesLocationUpdatesAutomatically = NO;
    lm.activityType = CLActivityTypeOther;
    @try {
        // Throws when no "location" background mode is declared by the process's Info.plist.
        lm.allowsBackgroundLocationUpdates = YES;
        lm.showsBackgroundLocationIndicator = YES;
    } @catch (NSException* e) {
        NSLog(@"background location unavailable: %@", e);
        diag::note("background location unavailable");
    }
}

bool current(swp::Position& out) {
    if (have) out = pos;
    return have;
}

std::string status_text() { return status; }

}  // namespace position
