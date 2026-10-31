# Requirements digest (agent-facing)

Source: the user's implementation brief (2026-10-30). Condensed checklist; status in progress.md.

- Watch: ESP-IDF + LVGL 9 on Waveshare ESP32-S3-Touch-AMOLED-2.06; companion: ESP-IDF on Freenove FNK0060. No Arduino.
- Same ESP-IDF for both (chosen: v5.5.5, Waveshare-validated). Nix flake dev shell.
- BLE GATT custom versioned binary protocol; watch central, companion peripheral; bonding persisted in NVS; auto-reconnect with bounded backoff; disconnect/reconnect/forget.
- Companion: Wi-Fi (configurable SSID/password, periodic scan with backoff), future LTE interface, TLS verification always.
- SD: SDMMC 1-bit (CLK14/CMD15/D0 2), never format on mount failure; format blank or conclusively non-FAT32 cards (log data loss); /music (2 GB), /maps (4 GB LRU), /memos; temp files + atomic rename.
- Music: Navidrome (Subsonic token auth + custom header), exactly one playlist (PROTOTYPE_PLAYLIST_ID), originals, MP3/FLAC/M4A, incremental + resumable sync, A2DP source + AVRCP to AirPods, watch music UI (lists, mini player, full player, shuffle/repeat, drag-down dismiss).
- Maps: OSM-derived, legally cacheable, VPS processing, zoom/pan, location, relative gyro heading (not a compass), POI search on tap, routing, offline cache, attribution. Dummy GPS 43°46'48.3"N 79°24'42.7"W.
- Steps (QMI8658, orientation independent, persisted sparingly); time from companion over BLE, local TZ, monotonic timers.
- Weather via Open-Meteo through the VPS (cache), app + home widget.
- Home face layout (HDG, BAT USB, date, big time, weather colours, wind, UV colour, precip text, music + launcher shortcuts); always-on with dimming, low refresh, burn-in shift.
- Every non-keyboard screen has Back + Home. Reusable keyboard (label, cursor, shift, 123, backspace, submit/cancel).
- Apps: Timer (10:00 default), Stopwatch, Calculator, Alarm, Calendar, Weather, Maps, Music, Voice memos (disabled), Settings (connections: companion/phone/AirPods/Wi-Fi; ntfy; vibration; device info).
- Notifications: companion subscribes to ntfy over HTTPS (at-least-once cursor, dedup), BLE delivery with ack, history/dismiss.
- Web: static TS/Vite, Web Bluetooth (Bluefy), no secrets, secure pairing required.
- Server: Rust/Axum, env config, tests, Containerfile, deployment docs.
- TODO markers: PROTOTYPE / HARDWARE / LTE / PHONE with explanation.
- Deliverables: builds, build/dist binaries + flash offsets, scripts, docs, checklist, progress.md.
