// App entry. Modern iOS expects scenes, older flows and hosts may not use them: whichever comes
// first creates the window.
#import <UIKit/UIKit.h>

#include "app.hpp"

namespace diag {
namespace {
NSString* const kEvents = @"diag.events";
NSString* const kLastTick = @"diag.last_tick";
constexpr NSUInteger kMaxEvents = 16;

// Run timestamps show when iOS stopped running the app: suspended or killed in the background.
std::string since_last_tick() {
    double last = [NSUserDefaults.standardUserDefaults doubleForKey:kLastTick];
    return last > 0 ? "; last ran " + std::to_string(static_cast<long>(NSDate.date.timeIntervalSince1970 - last)) + " s ago" : "";
}
}  // namespace

void note(const std::string& s) {
    NSDateFormatter* f = [NSDateFormatter new];
    f.dateFormat = @"HH:mm:ss";
    NSString* line = [NSString stringWithFormat:@"%@ %s", [f stringFromDate:NSDate.date], s.c_str()];
    NSMutableArray* a = [[NSUserDefaults.standardUserDefaults stringArrayForKey:kEvents] mutableCopy] ?: [NSMutableArray array];
    [a addObject:line];
    if (a.count > kMaxEvents) [a removeObjectsInRange:NSMakeRange(0, a.count - kMaxEvents)];
    [NSUserDefaults.standardUserDefaults setObject:a forKey:kEvents];
}

std::string text() { return str([[NSUserDefaults.standardUserDefaults stringArrayForKey:kEvents] componentsJoinedByString:@"\n"] ?: @""); }
}  // namespace diag

@interface SWScene : UIResponder <UIWindowSceneDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation SWScene
- (void)scene:(UIScene*)scene willConnectToSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    self.window = [[UIWindow alloc] initWithWindowScene:(UIWindowScene*)scene];
    self.window.rootViewController = ui::make_root();
    [self.window makeKeyAndVisible];
    player::attach_volume_view(self.window);
}
@end

@interface SWApp : UIResponder <UIApplicationDelegate>
@property(nonatomic, strong) UIWindow* window;
@end

@implementation SWApp

- (BOOL)application:(UIApplication*)app didFinishLaunchingWithOptions:(NSDictionary*)options {
    diag::note("launched" + diag::since_last_tick());
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    NSOperationQueue* main = NSOperationQueue.mainQueue;
    [nc addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:main usingBlock:^(NSNotification*) { diag::note("background"); }];
    [nc addObserverForName:UIApplicationWillEnterForegroundNotification object:nil queue:main usingBlock:^(NSNotification*) { diag::note("foreground" + diag::since_last_tick()); }];
    [nc addObserverForName:UIApplicationWillTerminateNotification object:nil queue:main usingBlock:^(NSNotification*) { diag::note("terminating"); }];
    [NSTimer scheduledTimerWithTimeInterval:5 repeats:YES block:^(NSTimer*) {
        [NSUserDefaults.standardUserDefaults setDouble:NSDate.date.timeIntervalSince1970 forKey:@"diag.last_tick"];
    }];
    net::start();
    player::start();
    position::start();
    cloud::start();
    ntfy::start();
    navidrome::start();
    dispatch::start();
    ble::start();
    [app beginReceivingRemoteControlEvents];
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        if (app.connectedScenes.count == 0 && !self.window) {
            self.window = [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
            self.window.rootViewController = ui::make_root();
            [self.window makeKeyAndVisible];
            player::attach_volume_view(self.window);
        }
    });
    return YES;
}

- (UISceneConfiguration*)application:(UIApplication*)app configurationForConnectingSceneSession:(UISceneSession*)session options:(UISceneConnectionOptions*)options {
    UISceneConfiguration* c = [[UISceneConfiguration alloc] initWithName:@"Default" sessionRole:session.role];
    c.delegateClass = SWScene.class;
    return c;
}

@end

int main(int argc, char** argv) {
    @autoreleasepool {
        return UIApplicationMain(argc, argv, nil, NSStringFromClass(SWApp.class));
    }
}
