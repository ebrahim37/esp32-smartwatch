# Architecture notes (agent-facing)

User-facing overview: docs/architecture.md. This file records design decisions and where
code lives so later sessions can continue quickly.

## Decisions
- Repo layout adapted to existing `firmware/{watch,companion}` placeholders instead of top-level `watch/`, `companion/`.
- BLE hosts: watch NimBLE (central + optional peripheral); companion Bluedroid (needed for Classic A2DP; NimBLE cannot coexist with Bluedroid Classic).
- Pairing: companion DisplayOnly static passkey, TODO(PROTOTYPE) fixed 616616 via CONFIG_COMPANION_PROTOTYPE_PASSKEY + pairing window; watch KeyboardDisplay. Links without SC+MITM are dropped on both sides.
- SWP framing over a byte stream; transport chunks to MTU-3; field visitor (`fields(A&)`) gives symmetric encode/decode.
- Watch UI thread model: producers update `app::State` + dirty bits; LVGL timer (100 ms) dispatches to the visible `ui::Screen::refresh`.
- Map tiles: VPS converts upstream raster to 16-colour 4-bit PNG; companion caches on SD (LRU at 8×8-tile block granularity, index persisted ≤15 min); watch caches 48 PNG blobs in PSRAM; LVGL lodepng decodes (image cache 3 MB).
- Notifications: server is the durable store (SQLite, dedup by source id); companion keeps an in-RAM pending map, forwards to watch, acks server only after the watch acks.
- Audio: esp_audio_simple_dec (container parse + decode) → ccore::PcmConverter (44.1 kHz s16 stereo) → SPSC ring in PSRAM → A2DP data callback. MP3 seek by byte offset; FLAC/M4A seek by decode-and-discard.
- Companion config in a separate `cfg` NVS partition so credentials can be provisioned without wiping bonds.

## Code map
- Protocol: shared/protocol/include/swp/{wire,frame,messages,transfer}.hpp
- Watch: firmware/watch/main/{app,ble,ui,ui/screens}; components/board (HAL), components/watch_core (pure logic).
- Companion: firmware/companion/main/{config,storage,net,lte,ble_server,dispatch,cloud,http,position}.cpp, main/music/{library,navidrome,a2dp,player}.cpp; components/companion_core (pure logic).
- Server: server/src/{app,config,weather,tiles,places,routing,mapdata,geo}.rs
- Web: web/src/{ble.ts,main.ts,protocol/*}
