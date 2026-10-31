// Static Web Bluetooth configuration app. No backend, no secrets bundled: every value the
// user types is sent only to the connected, paired device.
import { SwpConnection } from "./ble";
import {
  CompanionStatus, ConfigValue, decodeCompanionStatus, decodeConfigValue, decodeDeviceInfo, decodeError, decodeHello,
  decodeWatchSettings, decodeWifiStatus, DeviceInfo, encodeConfigGet, encodeConfigSet, encodeHello, encodeWatchSettings,
  encodeWifiConfig, MsgType, Role, WatchSettings, WifiStatus,
} from "./protocol/messages";
import { Frame } from "./protocol/wire";

const $ = (id: string) => document.getElementById(id)!;
const esc = (s: string) => s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" })[c]!);

let conn: SwpConnection | null = null;
let role = 0;
const config = new Map<string, ConfigValue>();
const log: string[] = [];

const NET_STATES = ["Disabled", "Not configured", "Searching", "Connecting", "Connected", "Failed"];
const SD_STATES = ["No card", "Mounted", "Error", "Formatting", "Unsupported"];
const CONFIG_HELP: Record<string, string> = {
  srv_url: "VPS helper base URL (https://...)", srv_token: "VPS helper device token", ntfy_url: "ntfy URL (https://...)", ntfy_token: "ntfy Auth header token", ntfy_topics: "ntfy topics (comma-separated)", nd_url: "Navidrome URL (https://...)",
  nd_user: "Navidrome user", nd_pass: "Navidrome password", nd_hdr: "Proxy auth header name", nd_hdr_val: "Proxy auth header value",
  nd_playlist: "Playlist id to sync (PROTOTYPE_PLAYLIST_ID)", tz: "POSIX time zone", dev_name: "Bluetooth name",
  wifi_ssid: "Home Wi-Fi SSID", wifi_pass: "Home Wi-Fi password",
};

function note(msg: string) {
  log.unshift(`${new Date().toLocaleTimeString()}  ${msg}`);
  log.length = Math.min(log.length, 50);
  renderDiag();
}

function show(id: string, html: string) {
  const el = $(id);
  el.innerHTML = html;
  el.classList.remove("hidden");
}

function renderDevice(d: DeviceInfo | null, s?: CompanionStatus) {
  const rows = [
    ["Device", d ? esc(d.name) : "?"],
    ["Type", role === Role.Watch ? "Watch" : role === Role.Companion ? "Companion" : d?.role === Role.Watch ? "Watch" : "Companion"],
    ["Firmware", d ? esc(d.firmware) : "?"],
    ["Hardware", d ? esc(d.hardware) : "?"],
    ["Bluetooth address", d ? d.mac : "?"],
  ];
  if (s) {
    rows.push(["SD card", `${SD_STATES[s.sdState] ?? "?"} - ${s.sdFreeMb} MB free of ${s.sdTotalMb} MB`]);
    rows.push(["Storage use", `music ${s.musicMb} MB, maps ${s.mapsMb} MB`]);
    rows.push(["Internet", `${NET_STATES[s.netState] ?? "?"}${s.serverOk ? " (server OK)" : ""}`]);
    rows.push(["Music sync", `${s.syncDone}/${s.syncTotal}`]);
    rows.push(["Pending notifications", String(s.notifPending)]);
    rows.push(["Uptime", `${Math.floor(s.uptimeS / 60)} min`]);
  }
  show("device", `<h2>Device</h2>${rows.map(([k, v]) => `<div class="row"><span>${k}</span><span class="muted">${v}</span></div>`).join("")}
    <div class="actions"><button id="refresh">Refresh</button><button id="disc" class="danger">Disconnect</button></div>`);
  $("refresh").onclick = () => requestAll();
  $("disc").onclick = () => conn?.disconnect();
}

function renderWifi(w: WifiStatus) {
  const ip = [w.ipv4 & 255, (w.ipv4 >>> 8) & 255, (w.ipv4 >>> 16) & 255, w.ipv4 >>> 24].join(".");
  show("wifi", `<h2>Companion home Wi-Fi</h2>
    <div class="row"><span>Status</span><span class="${w.state === 4 ? "ok" : "muted"}">${NET_STATES[w.state] ?? "?"}${w.state === 4 ? ` - ${ip}, ${w.rssi} dBm` : ""}</span></div>
    <label for="ssid">Network name (SSID)</label><input id="ssid" type="text" maxlength="32" autocomplete="off" value="${esc(w.ssid)}">
    <label for="pass">Password ${w.passwordSet ? "(saved; enter to replace)" : ""}</label><input id="pass" type="password" maxlength="63" autocomplete="new-password">
    <div class="actions"><button id="wifi-save" class="primary">Save Wi-Fi</button></div>`);
  $("wifi-save").onclick = async () => {
    const ssid = ($("ssid") as HTMLInputElement).value.trim();
    const pass = ($("pass") as HTMLInputElement).value;
    if (!ssid) return note("SSID is required");
    await conn!.send(MsgType.WifiConfigSet, encodeWifiConfig(ssid, pass));
    ($("pass") as HTMLInputElement).value = "";
    note("Wi-Fi settings sent");
  };
}

function renderWatch(s: WatchSettings) {
  show("watch", `<h2>Watch settings</h2>
    <div class="row"><label><input id="ntfy" type="checkbox" ${s.ntfyEnabled ? "checked" : ""}> ntfy notifications</label></div>
    <div class="row"><label><input id="vib" type="checkbox" ${s.vibrationEnabled ? "checked" : ""}> Vibration</label></div>
    <label for="bright">Brightness (%)</label><input id="bright" type="number" min="5" max="100" value="${s.brightness}">
    <label for="dim">Dim after (seconds)</label><input id="dim" type="number" min="5" max="600" value="${s.dimAfterS}">
    <label for="tz">Time zone (POSIX TZ)</label><input id="tz" type="text" maxlength="48" value="${esc(s.posixTz)}">
    <label for="steps">Step threshold (mg)</label><input id="steps" type="number" min="1050" max="2000" value="${s.stepThresholdMg}">
    <div class="actions"><button id="watch-save" class="primary">Save watch settings</button></div>`);
  $("watch-save").onclick = async () => {
    const num = (id: string, lo: number, hi: number) => Math.min(hi, Math.max(lo, Number(($(id) as HTMLInputElement).value) || lo));
    const next: WatchSettings = {
      ...s,
      ntfyEnabled: ($("ntfy") as HTMLInputElement).checked,
      vibrationEnabled: ($("vib") as HTMLInputElement).checked,
      brightness: num("bright", 5, 100),
      dimAfterS: num("dim", 5, 600),
      posixTz: ($("tz") as HTMLInputElement).value.trim() || "UTC0",
      stepThresholdMg: num("steps", 1050, 2000),
    };
    await conn!.send(MsgType.WatchSettings, encodeWatchSettings(next));
    note("Watch settings saved");
  };
}

function renderConfig() {
  const keys = [...config.values()].filter((v) => v.key !== "wifi_ssid" && v.key !== "wifi_pass");
  show("config", `<h2>Companion services</h2>
    <p class="muted small">Secrets are write-only: the device never sends them back.</p>
    ${keys.map((v) => `<label for="cfg-${v.key}">${esc(CONFIG_HELP[v.key] ?? v.key)} ${v.secret ? (v.isSet ? "(set)" : "(not set)") : ""}</label>
      <div class="actions" style="margin-top:0"><input id="cfg-${v.key}" type="${v.secret ? "password" : "text"}" autocomplete="off"
        value="${v.secret ? "" : esc(v.value)}" style="flex:1"><button data-key="${v.key}">Save</button></div>`).join("")}`);
  document.querySelectorAll<HTMLButtonElement>("#config button[data-key]").forEach((b) => {
    b.onclick = async () => {
      const key = b.dataset.key!;
      const input = $(`cfg-${key}`) as HTMLInputElement;
      await conn!.send(MsgType.ConfigSet, encodeConfigSet(key, input.value));
      if (input.type === "password") input.value = "";
      note(`Saved ${key}`);
    };
  });
}

function renderDiag() {
  if (!conn) return;
  const s = conn.stats;
  show("diag", `<h2>Diagnostics</h2>
    <div class="row"><span>Frames in / out</span><span class="muted">${s.framesIn} / ${s.framesOut}</span></div>
    <div class="row"><span>Bytes in / out</span><span class="muted">${s.bytesIn} / ${s.bytesOut}</span></div>
    <div class="row"><span>CRC errors</span><span class="muted">${conn.crcErrors}</span></div>
    <pre>${esc(log.join("\n"))}</pre>`);
}

function onFrame(f: Frame) {
  switch (f.type) {
    case MsgType.HelloAck: {
      const h = decodeHello(f.payload);
      if (h) { role = h.role; note(`Handshake OK with ${h.deviceName} (${h.firmware})`); requestAll(); }
      break;
    }
    case MsgType.DeviceInfo: { const d = decodeDeviceInfo(f.payload); if (d) { lastInfo = d; renderDevice(d, lastStatus); } break; }
    case MsgType.CompanionStatus: { const s = decodeCompanionStatus(f.payload); if (s) { lastStatus = s; renderDevice(lastInfo, s); } break; }
    case MsgType.WifiStatus: { const w = decodeWifiStatus(f.payload); if (w) renderWifi(w); break; }
    case MsgType.WatchSettings: { const s = decodeWatchSettings(f.payload); if (s) renderWatch(s); break; }
    case MsgType.ConfigValue: {
      const v = decodeConfigValue(f.payload);
      if (v) {
        if (v.status) note(`Rejected ${v.key} (invalid value)`);
        config.set(v.key, v);
        renderConfig();
      }
      break;
    }
    case MsgType.Error: { const e = decodeError(f.payload); if (e) note(`Device error: ${e.message}`); break; }
    default: break;
  }
  renderDiag();
}

let lastInfo: DeviceInfo | null = null;
let lastStatus: CompanionStatus | undefined;

async function requestAll() {
  if (!conn) return;
  await conn.send(MsgType.StatusRequest, new Uint8Array());
  await conn.send(MsgType.WifiStatusRequest, new Uint8Array());
  await conn.send(MsgType.ConfigGet, encodeConfigGet(""));
  if (role === Role.Watch) await conn.send(MsgType.WatchSettingsRequest, new Uint8Array());
}

async function connect() {
  const btn = $("connect") as HTMLButtonElement;
  btn.disabled = true;
  try {
    conn = await SwpConnection.request(); // must be called directly from the click handler
    btn.textContent = "Connecting... (accept the pairing prompt)";
    conn.onFrame = onFrame;
    conn.onDisconnect = () => {
      note("Disconnected");
      btn.disabled = false;
      btn.textContent = "Connect device";
      ["wifi", "watch", "config"].forEach((id) => $(id).classList.add("hidden"));
    };
    lastInfo = await conn.connect();
    renderDevice(lastInfo);
    await conn.send(MsgType.Hello, encodeHello({ role: Role.WebConfig, capabilities: 0, firmware: "webconfig-0.1", deviceName: navigator.userAgent.includes("Bluefy") ? "Bluefy" : "Browser" }));
    btn.textContent = "Connected";
  } catch (e) {
    note(`Connection failed: ${(e as Error).message}`);
    show("diag", `<h2>Diagnostics</h2><pre>${esc(log.join("\n"))}</pre>`);
    btn.disabled = false;
    btn.textContent = "Connect device";
  }
}

$("connect").onclick = connect;
$("support").textContent = navigator.bluetooth
  ? "Web Bluetooth available."
  : "Web Bluetooth is not available here. On iPhone use the Bluefy browser; on desktop use Chrome or Edge.";
