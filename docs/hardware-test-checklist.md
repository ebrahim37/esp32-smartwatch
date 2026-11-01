# Hardware test checklist

Nothing below has been run on real hardware yet; the firmware was only compiled. Work
through it in order and note the results in `agent/firmware/progress.md`.

## 0. Prepare
- [ ] `scripts/sw test` passes on the host.
- [ ] Server deployed (`docs/server.md`): `curl …/v1/status` shows `weather`, `tiles`, `search`, `routing` true.
- [ ] Helper/ntfy/Navidrome details at hand (entered on the watch under Settings → Server); 8 GB microSD available (back up anything on it: a non-FAT card **will be formatted**).

## 1. Companion bring-up (serial monitor at 115200)
- [ ] Flash, `--monitor`: boot log shows `SW Companion 0.1.0`, `BLE pairing passkey: 616616`.
- [ ] **No card:** log says `no card or card not responding`, no format attempt, retries every 30 s.
- [ ] **FAT32 card with files:** `probe: … kind=FAT32 -> existing FAT32 volume`; files untouched (check on a PC afterwards).
- [ ] **exFAT card (factory 64 GB style) or ext4:** log warns `FORMATTING ERASES ALL EXISTING FILES`, then mounts FAT32; `/music /maps /memos` exist.
- [ ] **Blank card** (`dd if=/dev/zero of=… bs=1M count=64`): formatted without the data-loss warning.
- [ ] Pull the card during operation and reboot: no format occurs.
- [ ] Provision cfg; Wi-Fi connects (`online via Wi-Fi`), SNTP `time synchronised`.
- [ ] Turn the AP off: log shows backoff (`next attempt in …`) growing to ~5 min, reconnects when AP returns.
- [ ] `heap:` log lines: internal free stays > 30 KB with Wi-Fi + BLE + A2DP connected.

## 2. Watch bring-up
- [ ] Flash, `--monitor`: display shows the home face within ~2 s; touch works; `QMI8658 at 0x6b`, `AXP2101 found`.
- [ ] Home: `BAT: USB` (no battery), `HDG` changes when rotating the watch flat, time `--:--` until synced.
- [ ] Wait 15 s: brightness dims; touch restores. After a minute the face shifts by a few pixels.
- [ ] Launcher grid opens all apps; Back and Home buttons work everywhere.
- [ ] Keyboard: shift (one-shot), 123/#+=, backspace, cursor blink, cancel ✕, submit ✓.
- [ ] Settings → Vibration toggle buzzes the motor (GPIO18).

## 3. Pairing and reconnection
- [ ] Companion unpaired (or within 5 min of boot): Watch Settings → Connections → Companion → Find & pair → select → keyboard asks for passkey → enter `616616` → Status `Connected`.
- [ ] Wrong passkey fails; no bond stored.
- [ ] Once paired and 5 min after boot, another device cannot pair (`pairing rejected: already paired`).
- [ ] Watch Forget → companion logs `unpaired on request of the watch`; pairing works again without a reboot.
- [ ] Reboot watch: reconnects automatically without passkey.
- [ ] Reboot companion: watch shows `Waiting to reconnect`, reconnects within ~1 min.
- [ ] Walk out of range for 5 min, come back: reconnects (backoff ≤ 5 min).
- [ ] Disconnect → stays disconnected; Reconnect → connects; Forget → `Not paired`.
- [ ] Time appears on the watch after the companion has NTP; date rollover at midnight resets steps.

## 4. Data features
- [ ] Weather app and home widget populate (description, low/current/high colours, wind, UV, precip text).
- [ ] Notifications: `curl -H "Authorization: Bearer <ntfy publish token>" -d test https://<ntfy>/<topic>` → watch banner + vibration within a few seconds; appears once even after a companion reboot (dedup); Dismiss/Clear work; ntfy toggle off → no banner.
- [ ] Publish to an ntfy topic → arrives on the watch.
- [ ] Maps: tiles load around the dummy location, `+`/`−`, drag to pan, recenter; `HDG … REL` cone rotates with the wrist; Search shows nearby places; pick → Walk → route line + banner; End clears.
- [ ] Companion log: `cloud: z/x/y net … ms: … B, ble … ms` per tile; a screen of tiles fills within a few seconds.
- [ ] A minute after boot (Wi-Fi up, no music, Maps closed): `prefetch: … KB …, N new tiles stored`; Settings → About shows maps MB growing.
- [ ] Search "coffee", "gas", a shop name: results within a second, nearest first. Route to a place 100+ km away: line drawn, banner shows distance.
- [ ] Pan Maps fast for a minute: no frozen bands; if the companion logs nothing but the watch logs `display flush not completed`, report it.
- [ ] Turn the companion's Wi-Fi off: previously viewed and prefetched tiles still load (SD cache).

- [ ] HTTPS proxy: a `fetch::request` to an https URL returns status and body (inline ≤3 KB and a larger one via transfer); an `http://` URL returns status 0.

## 5. Music / AirPods (unverified on AirPods — TODO(HARDWARE))
- [ ] Companion log lists `available playlist: id=…` if `nd_playlist` is unset; set it on the watch or the LAN settings page.
- [ ] Sync downloads originals into `/sdcard/music`; interrupt Wi-Fi mid-file → resumes (`Range`) later; removing a song from the playlist deletes it on the next sync.
- [ ] Watch Connections → AirPods → Search (AirPods in pairing mode, case button held) → select → `Connected`.
- [ ] Reboot companion: AirPods reconnect automatically.
- [ ] Play an MP3, a FLAC (16- and 24-bit) and an M4A (AAC and ALAC) track: audio is clean, no dropouts for 10 min with BLE connected and Wi-Fi up.
- [ ] Mini player, full player (`m:ss / -m:ss`), shuffle, repeat off/all/one, next/prev, drag-down dismiss.
- [ ] AirPods double-tap/press: play/pause/next reach the companion (AVRCP).
- [ ] Seek (MP3 jumps; FLAC/M4A seeks by fast-decoding: expect a short delay).
- [ ] Open Maps while music plays: tiles still load (slower), audio does not stutter.
- [ ] Timer completion with AirPods connected plays beeps and vibrates.

## 6. Web configuration
- [ ] Watch Settings → Web configuration → Open for 5 minutes → the watch shows `http://<companion-ip>/` and a countdown.
- [ ] The page loads from a phone/PC on the same Wi-Fi; secrets show only as saved/not set; a non-https URL is rejected.
- [ ] Saving a value is reflected on the watch (Settings → Server).
- [ ] Close now (or the countdown reaching 0) makes the page unreachable.
