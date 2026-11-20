// BLE central: finds the watch by its service (also when iOS already holds the link, e.g. connected
// in Bluetooth settings), keeps a pending connection to it, and carries SWP
// frames (TX notifications in, RX writes out). The watch demands an authenticated (passkey) link
// and starts the handshake with Hello once the link is encrypted and TX is subscribed.
#import <CoreBluetooth/CoreBluetooth.h>

#include <algorithm>
#include <deque>

#include "app.hpp"
#include "swp/frame.hpp"

namespace {

NSString* const kSavedPeer = @"ble.peer";
constexpr size_t kMaxWrite = 244;  // watch's ATT MTU (247) minus the ATT header
constexpr size_t kMaxQueued = 512;  // pieces; a full queue means the link is stuck

}  // namespace

@interface SWBle : NSObject <CBCentralManagerDelegate, CBPeripheralDelegate>
- (void)look;
- (void)use:(CBPeripheral*)p;
@end

namespace {

SWBle* me;
CBCentralManager* mgr;
CBPeripheral* peer;
CBCharacteristic* rx_chr;
CBCharacteristic* tx_chr;
swp::FrameDecoder decoder;
std::deque<NSData*> out;
uint16_t seq = 0;
bool subscribed = false;
bool stopped = false;  // Forget was pressed
bool pairing_lost = false;  // the watch dropped its bond: the status keeps asking for the fix
std::string status = "Starting";
std::string last_drop;
// Diagnostics for the current or last connection.
uint32_t dbg_pkts = 0, dbg_bytes = 0, dbg_frames = 0, dbg_bad = 0, dbg_sent = 0;
int dbg_last_type = -1;
std::string dbg_probe = "not sent";

CBUUID* uuid(NSString* s) { return [CBUUID UUIDWithString:s]; }
CBUUID* svc_uuid() { return uuid(@"7a1e0010-5c3b-4f8e-9d2a-6b1e5a7c0de0"); }
CBUUID* rx_uuid() { return uuid(@"7a1e0002-5c3b-4f8e-9d2a-6b1e5a7c0de0"); }
CBUUID* tx_uuid() { return uuid(@"7a1e0003-5c3b-4f8e-9d2a-6b1e5a7c0de0"); }

NSUUID* saved_peer() {
    NSString* s = [NSUserDefaults.standardUserDefaults stringForKey:kSavedPeer];
    return s ? [[NSUUID alloc] initWithUUIDString:s] : nil;
}

size_t write_len() {
    if (!peer) return 20;
    size_t n = [peer maximumWriteValueLengthForType:CBCharacteristicWriteWithoutResponse];
    return std::clamp<size_t>(n, 20, kMaxWrite);
}

void pump_out() {
    while (peer && rx_chr && !out.empty() && peer.canSendWriteWithoutResponse) {
        [peer writeValue:out.front() forCharacteristic:rx_chr type:CBCharacteristicWriteWithoutResponse];
        ++dbg_sent;
        out.pop_front();
    }
}

void link_lost() {
    bool was_up = subscribed;
    subscribed = false;
    rx_chr = tx_chr = nil;
    out.clear();
    decoder.reset();
    if (was_up) dispatch::on_link_down();
}

}  // namespace

@implementation SWBle

- (void)look {
    if (stopped || mgr.state != CBManagerStatePoweredOn || peer.state == CBPeripheralStateConnected) return;
    if (CBPeripheral* p = [mgr retrieveConnectedPeripheralsWithServices:@[svc_uuid()]].firstObject) {
        [self use:p];
        return;
    }
    if (!pairing_lost) status = saved_peer() ? "Waiting for the watch" : "Looking for a watch";
    if (NSUUID* id = saved_peer()) {
        // A pending connection has no timeout: iOS completes it when the watch comes in range.
        if (CBPeripheral* p = [mgr retrievePeripheralsWithIdentifiers:@[id]].firstObject) [self use:p];
    }
    // Also scan: the saved identifier is useless once the bond was forgotten and made again.
    [mgr scanForPeripheralsWithServices:@[svc_uuid()] options:nil];
}

- (void)use:(CBPeripheral*)p {
    if (peer && ![peer.identifier isEqual:p.identifier]) [mgr cancelPeripheralConnection:peer];
    peer = p;
    peer.delegate = self;
    [mgr connectPeripheral:peer options:nil];
}

- (void)centralManagerDidUpdateState:(CBCentralManager*)c {
    switch (c.state) {
        case CBManagerStatePoweredOn: [self look]; break;
        case CBManagerStateUnauthorized: status = "Bluetooth permission denied"; break;
        case CBManagerStatePoweredOff: status = "Bluetooth is off"; link_lost(); break;
        case CBManagerStateUnsupported: status = "Bluetooth LE not available"; break;
        default: break;
    }
}

- (void)centralManager:(CBCentralManager*)c didDiscoverPeripheral:(CBPeripheral*)p advertisementData:(NSDictionary*)ad RSSI:(NSNumber*)rssi {
    [mgr stopScan];
    if (!pairing_lost) status = "Connecting";
    [self use:p];
}

- (void)centralManager:(CBCentralManager*)c didConnectPeripheral:(CBPeripheral*)p {
    [mgr stopScan];
    diag::note("link connected");
    dbg_pkts = dbg_bytes = dbg_frames = dbg_bad = dbg_sent = 0;
    dbg_last_type = -1;
    dbg_probe = "not sent";
    if (!pairing_lost) status = "Securing the link (enter the code shown on the watch if asked)";
    [p discoverServices:@[svc_uuid()]];
}

- (void)centralManager:(CBCentralManager*)c didFailToConnectPeripheral:(CBPeripheral*)p error:(NSError*)e {
    NSLog(@"connect failed: %@", e);
    last_drop = "Connect failed: " + str(e.localizedDescription ?: @"unknown") + " (" + std::to_string(e.code) + ")";
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 2 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self look]; });
}

- (void)centralManager:(CBCentralManager*)c didDisconnectPeripheral:(CBPeripheral*)p error:(NSError*)e {
    NSLog(@"disconnected: %@", e);
    last_drop = e ? str(e.localizedDescription) + " (" + std::to_string(e.code) + ")" : "Closed by the app";
    diag::note("link down: " + last_drop);
    link_lost();
    // The watch forgot this phone: only the user can clear iOS's side of the bond.
    if (e.code == CBErrorPeerRemovedPairingInformation) {
        pairing_lost = true;
        status = "Pairing lost: forget the watch in iOS Bluetooth settings";
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (pairing_lost ? 5 : 1) * NSEC_PER_SEC), dispatch_get_main_queue(), ^{ [self look]; });
}

- (void)peripheral:(CBPeripheral*)p didDiscoverServices:(NSError*)e {
    for (CBService* s in p.services)
        if ([s.UUID isEqual:svc_uuid()]) {
            [p discoverCharacteristics:@[rx_uuid(), tx_uuid()] forService:s];
            return;
        }
    NSLog(@"watch service missing: %@", e);
    [mgr cancelPeripheralConnection:p];
}

- (void)peripheral:(CBPeripheral*)p didDiscoverCharacteristicsForService:(CBService*)s error:(NSError*)e {
    for (CBCharacteristic* c in s.characteristics) {
        if ([c.UUID isEqual:rx_uuid()]) rx_chr = c;
        if ([c.UUID isEqual:tx_uuid()]) tx_chr = c;
    }
    if (!rx_chr || !tx_chr) {
        [mgr cancelPeripheralConnection:p];
        return;
    }
    [p setNotifyValue:YES forCharacteristic:tx_chr];
}

- (void)peripheral:(CBPeripheral*)p didUpdateNotificationStateForCharacteristic:(CBCharacteristic*)c error:(NSError*)e {
    if (e || !c.isNotifying) {
        NSLog(@"subscribe failed: %@", e);
        [mgr cancelPeripheralConnection:p];
        return;
    }
    subscribed = true;
    pairing_lost = false;
    [NSUserDefaults.standardUserDefaults setObject:p.identifier.UUIDString forKey:kSavedPeer];
    status = "Waiting for the watch's hello";
    dispatch::on_link_up();
    // iOS may ignore the watch's security request on a bonded link. RX needs an authenticated link,
    // so an empty write makes iOS encrypt now; the watch's Hello only comes after that.
    dbg_probe = "sent";
    [p writeValue:[NSData data] forCharacteristic:rx_chr type:CBCharacteristicWriteWithResponse];
}

- (void)peripheral:(CBPeripheral*)p didUpdateValueForCharacteristic:(CBCharacteristic*)c error:(NSError*)e {
    if (e || ![c.UUID isEqual:tx_uuid()] || !c.value.length) return;  // a repeated discovery replaces the objects
    ++dbg_pkts;
    dbg_bytes += c.value.length;
    decoder.feed(static_cast<const uint8_t*>(c.value.bytes), c.value.length);
    swp::Frame f;
    for (;;) {
        auto r = decoder.next(f);
        if (r == swp::FrameDecoder::Result::NeedMore) break;
        if (r == swp::FrameDecoder::Result::Error) ++dbg_bad;
        if (r == swp::FrameDecoder::Result::Frame) {
            ++dbg_frames;
            dbg_last_type = f.type;
            dispatch::on_frame(f);
        }
    }
    if (dispatch::watch_ready()) status = "Connected to " + str(p.name ?: @"the watch");
}

- (void)peripheral:(CBPeripheral*)p didWriteValueForCharacteristic:(CBCharacteristic*)c error:(NSError*)e {
    dbg_probe = e ? str(e.localizedDescription) + " (" + std::to_string(e.code) + ")" : "ok";
}

// Service Changed: the old characteristics are dead, so find and subscribe to them again.
- (void)peripheral:(CBPeripheral*)p didModifyServices:(NSArray<CBService*>*)invalidated {
    link_lost();
    [p discoverServices:@[svc_uuid()]];
}

- (void)peripheralIsReadyToSendWriteWithoutResponse:(CBPeripheral*)p {
    pump_out();
    cloud::pump();
}

@end

namespace ble {

void start() {
    me = [SWBle new];
    mgr = [[CBCentralManager alloc] initWithDelegate:me queue:dispatch_get_main_queue()];
}

void forget() {
    stopped = true;
    pairing_lost = false;
    [NSUserDefaults.standardUserDefaults removeObjectForKey:kSavedPeer];
    [mgr stopScan];
    if (peer) [mgr cancelPeripheralConnection:peer];
    peer = nil;
    link_lost();
    status = "Not paired";
}

void connect() {
    stopped = false;
    [me look];
}

bool up() { return subscribed && rx_chr; }
bool has_peer() { return saved_peer() != nil; }
std::string status_text() { return status; }
std::string last_drop_text() { return last_drop; }

std::string debug_text() {
    char b[160];
    snprintf(b, sizeof b, "in %u pkts %u B, frames %u (last 0x%02x), bad %u; out %u; encrypt write: %s", dbg_pkts, dbg_bytes, dbg_frames, dbg_last_type < 0 ? 0 : dbg_last_type, dbg_bad, dbg_sent, dbg_probe.c_str());
    return b;
}

bool send_frame(uint8_t type, const std::vector<uint8_t>& payload) {
    if (![NSThread isMainThread]) {
        std::vector<uint8_t> copy = payload;
        dispatch_async(dispatch_get_main_queue(), ^{ send_frame(type, copy); });
        return true;
    }
    if (!up() || out.size() > kMaxQueued) return false;
    std::vector<uint8_t> frame;
    if (!swp::encode_frame(type, seq++, payload, frame)) return false;
    size_t piece = write_len();
    for (size_t i = 0; i < frame.size(); i += piece)
        out.push_back([NSData dataWithBytes:frame.data() + i length:std::min(piece, frame.size() - i)]);
    pump_out();
    return true;
}

size_t queued() { return out.size(); }

uint16_t chunk_size() {
    // One chunk per write: frame header + CRC (10) + chunk header (8).
    return static_cast<uint16_t>(std::clamp<int>(static_cast<int>(write_len()) - 18, 20, 480));
}

}  // namespace ble
