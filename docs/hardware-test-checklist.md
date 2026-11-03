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
- [ ] Watch face: date + time | LINK / NET; weather row (icon lined up with date/time, wind and outlook right-justified, no divider); steps | battery, separated by dividers with the vertical dividers lined up. LINK green when the companion link is up (orange: paired, not connected). NET green within 90 s of an internet exchange through the companion (HTTPS, ntfy keepalive, SNTP; not LAN Navidrome), grey otherwise.
- [ ] Hold the bell on the watch face: vibrates, icon becomes a crossed-out bell on grey (Do Not Disturb); ntfy notifications are then neither shown, vibrated nor listed (still acknowledged, so they are not redelivered). Hold again to turn it off; survives a reboot. Tap still opens Notifications.
- [ ] Settings → Passcode: set a 4-digit passcode (entered twice). Apps → Lock locks immediately (without a passcode it says to set one in Settings) behind a keypad; wrong code vibrates, 5 wrong codes lock out for 30 s; right code unlocks. Power-cycle while locked → still locked. Change / turn off the passcode needs the current one.
- [ ] Watch face dimmed after inactivity: only date, time, weather icon + temperature, rain text if coming or UV if high, battery; no dividers or buttons. Touch restores the full face.
- [ ] Weather app top row matches the watch-face row; condition icon (moon at night), high (red) / low (blue) temperatures, "Rain in N hrs" (blue) or, when dry, "N UV" (red from UV 6) over "Wind N km/h". Hourly shows "11 PM"-style times with icons; 7-Day rows show icons instead of words.
- [ ] Notifications: `curl -H "Authorization: Bearer <ntfy publish token>" -d test https://<ntfy>/<topic>` → watch banner + vibration within a few seconds; appears once even after a companion reboot (dedup); Dismiss/Clear work; ntfy toggle off → no banner.
- [ ] Publish to an ntfy topic → arrives on the watch.
- [ ] Maps: tiles load around the dummy location, `+`/`−`, drag to pan, recenter; `HDG … REL` cone rotates with the wrist; Search shows Search places, Nearby (with a count; opens its own list) and Favourites; typed searches open a results page; pick → the last button toggles the favourite (survives reboot) → Walk → route line + banner; Back from directions returns to the Walk/Cycle/Drive choice with the route cleared; End clears.
- [ ] Companion log: `cloud: z/x/y net … ms: … B, ble … ms` per tile; a screen of tiles fills within a few seconds.
- [ ] A minute after boot (Wi-Fi up, no music, Maps closed): `prefetch: … KB …, N new tiles stored`; Settings → About shows maps MB growing.
- [ ] Search "coffee", "gas", a shop name: results within a second, nearest first. Route to a place 100+ km away: line drawn, banner shows distance.
- [ ] Pan Maps fast for a minute: no frozen bands; if the companion logs nothing but the watch logs `display flush not completed`, report it.
- [ ] Turn the companion's Wi-Fi off: previously viewed and prefetched tiles still load (SD cache).

- [ ] HTTPS proxy: a `fetch::request` to an https URL returns status and body (inline ≤3 KB and a larger one via transfer); an `http://` URL returns status 0.

## 5. Music / AirPods (unverified on AirPods — TODO(HARDWARE))
- [ ] Sync covers every Navidrome playlist, then the rest of the library up to 3 GiB (`navidrome: N playlist(s), M tracks: keep …, download …, over quota …`); playlist tracks download first.
- [ ] Music: Search (song or artist, downloaded songs only; Arabic Yeh/Kaf match either form) → pick → plays on repeat one. Shuffle all → random start, shuffle + repeat all. After a reboot, play on the watch or a stem press does the same as Shuffle all.
- [ ] Playlist rows: `N songs · h:mm:ss` from Navidrome's exact playlist total.
- [ ] Arabic/Persian titles and artists render right-to-left with joined letters (playlist rows, now playing, up next).
- [ ] Now Playing: progress and volume sliders stay where released (no snap back); elapsed and remaining tick at different moments; Up next shows the next 4 (new random picks after each switch when shuffled), tap one to skip there; drag down at the top dismisses.
- [ ] Internal RAM stays healthy with a large library (`main: heap: internal … (min …)`, no `ALLOC FAIL`).
- [ ] Sync downloads each track transcoded to 128 kbps MP3 (`stream?format=mp3`) into `/sdcard/music` (`navidrome: track … KB in … ms`); interrupt Wi-Fi mid-file → that track starts over later; older originals of the same tracks are deleted once replaced; removing a song from the playlist deletes it on the next sync.
- [ ] Watch Connections → AirPods → Search (AirPods in pairing mode, case button held) → select → `Connected`.
- [ ] AirPods in use by the iPhone: companion never takes them on its own (not at boot, not after the phone takes them back). Play on the watch → companion connects (`connecting to … for playback`); pause 1 min → `releasing …`, AirPods free for the phone.
- [ ] Play an MP3, a FLAC (16- and 24-bit) and an M4A (AAC and ALAC) track: audio is clean, no dropouts for 10 min with BLE connected and Wi-Fi up.
- [ ] Mini player, full player (`m:ss / -m:ss`), shuffle, repeat off/all/one, next/prev, drag-down dismiss.
- [x] AirPods stem press: single = play/pause (always toggles), double = next, triple = previous (`headphone button 0x.. pressed`; verified on AirPods).
- [ ] Tap play on the watch twice while the AirPods connect: playback still starts (`ignoring play/pause tapped while starting`).
- [ ] Drag the progress slider: times follow the finger, playback jumps there on release (MP3 jumps; FLAC/M4A seek by fast-decoding: expect a short delay).
- [ ] Volume slider: AirPods (AVRCP absolute volume, `headphones support absolute volume`) change loudness; headphones without it are scaled in software; volume changed on the AirPods/iPhone moves the slider.
- [ ] Open Maps while music plays: tiles still load (slower), audio does not stutter.
- [ ] Timer completion with AirPods connected plays beeps and vibrates.

## 6. Web configuration
- [ ] Watch Settings → Web configuration → Open for 5 minutes → the watch shows `http://<companion-ip>/` and a countdown.
- [ ] The page loads from a phone/PC on the same Wi-Fi; secrets show only as saved/not set; a non-https URL is rejected.
- [ ] Saving a value is reflected on the watch (Settings → Server).
- [ ] Close now (or the countdown reaching 0) makes the page unreachable.
