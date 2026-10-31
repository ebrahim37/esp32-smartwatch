# System architecture

```
 ┌──────────── Watch (ESP32-S3) ───────────┐   BLE (SWP, bonded)   ┌────────── Companion (ESP32) ──────────┐  HTTPS  ┌──── VPS helper (Rust) ────┐
 │ LVGL 9 UI ─ nav stack ─ screens         │◄─────────────────────►│ GATT server ─ dispatcher              │◄──────►│ weather (Open-Meteo)      │
 │ app::State (mutex + dirty bits)         │  watch = central      │ cloud worker: weather/maps; ntfy sub  │ bearer │ tiles (recolour+quantise) │
 │ services: IMU steps/heading, timer,     │                       │ net: Wi-Fi provider (+ LTE stub)      │  token │ search (Photon/Overpass)  │
 │   stopwatch, alarms, power              │  peripheral (5-min    │ storage: SD FAT32, quotas, tile LRU   │        │ routing (OSRM)            │
 │ swlink: framing, TX queue, transfers    │  window) for web app  │ music: Navidrome sync, decoder,       │        │ map data (Geofabrik→OSRM) │
 │ board HAL: BSP display/touch, AXP2101,  │                       │   A2DP source + AVRCP ──► AirPods     │        └───────────────────────────┘
 │   QMI8658, motor                        │                       │ position: simulated (→ GNSS)          │   Navidrome (Subsonic) via Caddy
 └─────────────────────────────────────────┘                       └───────────────────────────────────────┘
                     ▲  Web Bluetooth (Bluefy / Chrome), static site on Cloudflare Pages
```

## Responsibilities

* **Watch**: UI, touch, local apps (timer, stopwatch, alarms, calculator, calendar), steps,
  relative heading, timekeeping (software clock synchronised over BLE; no RTC), notification
  history. No Wi-Fi.
* **Companion**: all connectivity (Wi-Fi now, `TODO(LTE)` EG912U later), SD storage, map
  cache, music library + playback to AirPods, position (`TODO(PROTOTYPE)` dummy fix).
* **VPS helper**: heavy lifting and upstream credentials: weather summaries, tile
  conversion, POI search, routing, on-demand OSM map data.

`TODO(PHONE)`: a phone app may later replace the companion as provider; it would speak the
same SWP protocol (`Role::Phone`). The watch only depends on SWP messages, not on who sends them.

## Threading

| Device | Task | Core / prio | Role |
|--------|------|-------------|------|
| Watch | LVGL (esp_lvgl_port) | 1 / 4 | all UI; reads `app::State` under lock |
| Watch | NimBLE host | 0 | GAP/GATT events, frame decode, state updates |
| Watch | `ble_tx` | 0 / 8 | chunked writes (non-blocking for callers) |
| Watch | `ble_mgr` | 0 / 5 | reconnect scheduler, pairing commands |
| Watch | `imu` 50 Hz, `svc` 4 Hz | 1 / 6, 3 | steps/heading; timer, alarms, persistence |
| Companion | `decoder` | 1 / 18 | SD → decoder → PCM ring (0.74 s, PSRAM) |
| Companion | BT (A2DP callback) | 0 | pops PCM ring, never blocks |
| Companion | `ble_tx` | 0 / 9 | notifications with congestion backoff |
| Companion | `net`, `cloud`, `storage`, `nd_sync` | 0 / 4,4,3,2 | network and SD work, lowest for sync |

Audio first: music sync pauses while playing, map transfers keep fewer frames in flight
while A2DP streams, and the watch is told (heartbeat flag) to avoid bulk requests.

## Persistence and flash wear

* Watch `nvs_user`: settings, peer, alarms (on change), notification history (debounced
  30 s), step checkpoint (≤ every 10 min and at midnight). NimBLE bonds in `nvs`.
* Companion `cfg`: configuration only; `nvs`: Bluetooth bonds, remembered A2DP sink.
* SD card: music, `library.json` (after sync), map tiles, LRU index (≤ every 15 min).
