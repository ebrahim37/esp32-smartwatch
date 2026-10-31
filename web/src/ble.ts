// Web Bluetooth transport for SWP.
//
// Browser notes (see docs/web-config.md):
//  * Every requestDevice() must run inside a user gesture (button click).
//  * iOS Safari has no Web Bluetooth; use Bluefy. Pairing (passkey) is handled by the iOS
//    system dialog the first time an encrypted characteristic is accessed.
//  * The ATT MTU is not exposed, so writes are chunked to 20 bytes (always valid) and use
//    write-with-response for reliable ordering.
import { decodeDeviceInfo, DeviceInfo } from "./protocol/messages";
import { encodeFrame, Frame, FrameDecoder, INFO_UUID, RX_UUID, SERVICE_UUID, TX_UUID } from "./protocol/wire";

export class SwpConnection {
  private rx?: BluetoothRemoteGATTCharacteristic;
  private decoder = new FrameDecoder();
  private seq = 1;
  private writeChain: Promise<void> = Promise.resolve();
  onFrame: (f: Frame) => void = () => {};
  onDisconnect: () => void = () => {};
  stats = { framesIn: 0, framesOut: 0, bytesIn: 0, bytesOut: 0 };

  constructor(public device: BluetoothDevice) {}

  static async request(): Promise<SwpConnection> {
    if (!navigator.bluetooth) throw new Error("Web Bluetooth is not available in this browser. On iPhone, open this page in Bluefy.");
    const device = await navigator.bluetooth.requestDevice({ filters: [{ services: [SERVICE_UUID] }] });
    return new SwpConnection(device);
  }

  /** Connects and reads the unauthenticated identity characteristic. */
  async connect(): Promise<DeviceInfo | null> {
    const server = await this.device.gatt!.connect();
    this.device.addEventListener("gattserverdisconnected", () => this.onDisconnect());
    const svc = await server.getPrimaryService(SERVICE_UUID);
    let info: DeviceInfo | null = null;
    try {
      const v = await (await svc.getCharacteristic(INFO_UUID)).readValue();
      info = decodeDeviceInfo(new Uint8Array(v.buffer, v.byteOffset, v.byteLength));
    } catch {
      /* older firmware without the info characteristic */
    }
    this.rx = await svc.getCharacteristic(RX_UUID);
    const tx = await svc.getCharacteristic(TX_UUID);
    // Subscribing requires an encrypted, MITM-authenticated link: this triggers pairing.
    await tx.startNotifications();
    tx.addEventListener("characteristicvaluechanged", (ev) => {
      const v = (ev.target as BluetoothRemoteGATTCharacteristic).value!;
      const bytes = new Uint8Array(v.buffer, v.byteOffset, v.byteLength);
      this.stats.bytesIn += bytes.length;
      this.decoder.feed(bytes);
      for (let f = this.decoder.next(); f; f = this.decoder.next()) {
        this.stats.framesIn++;
        this.onFrame(f);
      }
    });
    return info;
  }

  get crcErrors() { return this.decoder.crcErrors; }

  send(type: number, payload: Uint8Array): Promise<void> {
    const frame = encodeFrame(type, this.seq++ & 0xffff, payload);
    this.stats.framesOut++;
    // Serialise writes: GATT allows one outstanding operation at a time.
    this.writeChain = this.writeChain.then(async () => {
      for (let o = 0; o < frame.length; o += 20) {
        const chunk = frame.slice(o, o + 20);
        // Some Web Bluetooth implementations (older Bluefy) only provide writeValue().
        if (typeof (this.rx as { writeValueWithResponse?: unknown }).writeValueWithResponse === "function") await this.rx!.writeValueWithResponse(chunk);
        else await this.rx!.writeValue(chunk);
        this.stats.bytesOut += chunk.length;
      }
    });
    return this.writeChain;
  }

  disconnect() { this.device.gatt?.disconnect(); }
}
