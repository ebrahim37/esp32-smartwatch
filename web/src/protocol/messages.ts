// Subset of the SWP message catalogue used by the configuration app.
import { Reader, Writer } from "./wire";

export const MsgType = {
  Hello: 0x01, HelloAck: 0x02, Heartbeat: 0x03, Error: 0x04,
  WifiConfigSet: 0x60, WifiStatusRequest: 0x61, WifiStatus: 0x62,
  ConfigSet: 0x70, ConfigGet: 0x71, ConfigValue: 0x72,
  StatusRequest: 0x78, CompanionStatus: 0x79, DeviceInfo: 0x7a,
  WatchSettings: 0x7b, WatchSettingsRequest: 0x7c,
} as const;

export const Role = { Watch: 1, Companion: 2, WebConfig: 3, Phone: 4 } as const;

export interface Hello { role: number; capabilities: number; firmware: string; deviceName: string; }
export const encodeHello = (h: Hello) =>
  new Writer().u8(1).u8(1).u8(h.role).u32(h.capabilities).str8(h.firmware).str8(h.deviceName).done();
export function decodeHello(b: Uint8Array): Hello | null {
  const r = new Reader(b);
  r.u8(); r.u8();
  const h = { role: r.u8(), capabilities: r.u32(), firmware: r.str8(), deviceName: r.str8() };
  return r.failed ? null : h;
}

export interface ErrorMsg { code: number; refType: number; message: string; }
export function decodeError(b: Uint8Array): ErrorMsg | null {
  const r = new Reader(b);
  const e = { code: r.u8(), refType: r.u8(), message: r.str8() };
  return r.failed ? null : e;
}

export const encodeWifiConfig = (ssid: string, password: string) => new Writer().str8(ssid).str8(password).done();

export interface WifiStatus { state: number; provider: number; ssid: string; rssi: number; ipv4: number; passwordSet: boolean; internetOk: boolean; }
export function decodeWifiStatus(b: Uint8Array): WifiStatus | null {
  const r = new Reader(b);
  const w = { state: r.u8(), provider: r.u8(), ssid: r.str8(), rssi: r.i8(), ipv4: r.u32(), passwordSet: r.u8() !== 0, internetOk: r.u8() !== 0 };
  return r.failed ? null : w;
}

export const encodeConfigSet = (key: string, value: string) => new Writer().str8(key).str16(value, 512).done();
export const encodeConfigGet = (key: string) => new Writer().str8(key).done();
export interface ConfigValue { key: string; isSet: boolean; secret: boolean; value: string; status: number; }
export function decodeConfigValue(b: Uint8Array): ConfigValue | null {
  const r = new Reader(b);
  const v = { key: r.str8(), isSet: r.u8() !== 0, secret: r.u8() !== 0, value: r.str16(512), status: r.u8() };
  return r.failed ? null : v;
}

export interface CompanionStatus {
  firmware: string; uptimeS: number; sdState: number; sdTotalMb: number; sdFreeMb: number; musicMb: number; mapsMb: number;
  netState: number; netProvider: number; serverOk: boolean; syncState: number; syncDone: number; syncTotal: number;
  heapFree: number; psramFree: number; notifPending: number; lastNotifPoll: number;
}
export function decodeCompanionStatus(b: Uint8Array): CompanionStatus | null {
  const r = new Reader(b);
  const s = {
    firmware: r.str8(), uptimeS: r.u32(), sdState: r.u8(), sdTotalMb: r.u32(), sdFreeMb: r.u32(), musicMb: r.u32(), mapsMb: r.u32(),
    netState: r.u8(), netProvider: r.u8(), serverOk: r.u8() !== 0, syncState: r.u8(), syncDone: r.u16(), syncTotal: r.u16(),
    heapFree: r.u32(), psramFree: r.u32(), notifPending: r.u16(), lastNotifPoll: r.u32(),
  };
  return r.failed ? null : s;
}

export interface DeviceInfo { role: number; name: string; firmware: string; hardware: string; mac: string; protoVersion: number; }
export function decodeDeviceInfo(b: Uint8Array): DeviceInfo | null {
  const r = new Reader(b);
  const role = r.u8(), name = r.str8(), firmware = r.str8(), hardware = r.str8();
  const mac = Array.from(r.raw(6)).map((x) => x.toString(16).padStart(2, "0")).join(":").toUpperCase();
  const d = { role, name, firmware, hardware, mac, protoVersion: r.u8() };
  return r.failed ? null : d;
}

export interface WatchSettings {
  ntfyEnabled: boolean; vibrationEnabled: boolean; brightness: number; dimBrightness: number; dimAfterS: number; posixTz: string; stepThresholdMg: number;
}
export const encodeWatchSettings = (s: WatchSettings) =>
  new Writer().u8(s.ntfyEnabled ? 1 : 0).u8(s.vibrationEnabled ? 1 : 0).u8(s.brightness).u8(s.dimBrightness)
    .u16(s.dimAfterS).str8(s.posixTz).u16(s.stepThresholdMg).done();
export function decodeWatchSettings(b: Uint8Array): WatchSettings | null {
  const r = new Reader(b);
  const s = { ntfyEnabled: r.u8() !== 0, vibrationEnabled: r.u8() !== 0, brightness: r.u8(), dimBrightness: r.u8(), dimAfterS: r.u16(), posixTz: r.str8(), stepThresholdMg: r.u16() };
  return r.failed ? null : s;
}
