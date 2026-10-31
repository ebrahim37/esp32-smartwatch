import { describe, expect, it } from "vitest";
import fixtures from "./fixtures.json";
import { decodeConfigValue, decodeWatchSettings, decodeWifiStatus, encodeHello, encodeWifiConfig, MsgType } from "./messages";
import { crc16, encodeFrame, FrameDecoder } from "./wire";

const hex = (b: Uint8Array) => Array.from(b).map((x) => x.toString(16).padStart(2, "0")).join("");
const unhex = (s: string) => Uint8Array.from(s.match(/../g)!.map((x) => parseInt(x, 16)));

describe("SWP codec matches the C++ reference", () => {
  it("crc16 known vector", () => expect(crc16(new TextEncoder().encode("123456789"))).toBe(0x29b1));
  it("encodes Hello identically", () => {
    const f = encodeFrame(MsgType.Hello, 1, encodeHello({ role: 3, capabilities: 0x140, firmware: "web-0.1", deviceName: "Bluefy" }));
    expect(hex(f)).toBe(fixtures.hello);
  });
  it("encodes WifiConfigSet with UTF-8 identically", () => {
    expect(hex(encodeFrame(MsgType.WifiConfigSet, 2, encodeWifiConfig("HomeNet", "päss")))).toBe(fixtures.wifi_config_set);
  });
  it("decodes fragmented frames with garbage", () => {
    const d = new FrameDecoder();
    const stream = new Uint8Array([0x00, 0xa5, ...unhex(fixtures.wifi_status), ...unhex(fixtures.config_value), ...unhex(fixtures.watch_settings)]);
    const frames = [];
    for (let i = 0; i < stream.length; i += 7) {
      d.feed(stream.subarray(i, i + 7));
      for (let f = d.next(); f; f = d.next()) frames.push(f);
    }
    expect(frames.map((f) => f.type)).toEqual([MsgType.WifiStatus, MsgType.ConfigValue, MsgType.WatchSettings]);
    const ws = decodeWifiStatus(frames[0].payload)!;
    expect(ws).toMatchObject({ state: 4, ssid: "HomeNet", rssi: -61, passwordSet: true });
    expect(decodeConfigValue(frames[1].payload)).toMatchObject({ key: "srv_token", isSet: true, secret: true, value: "" });
    expect(decodeWatchSettings(frames[2].payload)).toMatchObject({ ntfyEnabled: true, vibrationEnabled: true, posixTz: "EST5EDT,M3.2.0,M11.1.0" });
  });
  it("rejects corrupted frames", () => {
    const d = new FrameDecoder();
    const b = unhex(fixtures.wifi_status);
    b[9] ^= 0xff;
    d.feed(b);
    expect(d.next()).toBeNull();
    expect(d.crcErrors).toBe(1);
  });
});
