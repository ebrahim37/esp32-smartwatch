# ESP32 smartwatch ecosystem

Custom smartwatch (ESP32-S3, LVGL 9) + wireless companion (ESP32: Wi-Fi, SD, AirPods audio)
+ Rust VPS helper + optional iOS app that replaces the companion. Prototype hardware: Waveshare
ESP32-S3-Touch-AMOLED-2.06 (watch) and Freenove FNK0060 ESP32-WROVER (companion).

```
firmware/watch/       ESP-IDF 5.5.5 watch firmware (board HAL in components/board, logic in components/watch_core)
firmware/companion/   ESP-IDF 5.5.5 companion firmware (logic in components/companion_core)
shared/protocol/      SWP: versioned binary BLE protocol (C++17), docs/protocol.md
server/               Rust/Axum VPS helper (container: see server/README.md)
phone/ios/            iOS companion app (Objective-C++, built on Linux, runs in LiveContainer): docs/ios-app.md
tests/host/           host unit tests for all hardware-independent C++ code
scripts/sw            build, test, package, flash, monitor, fonts, iOS SDK + ipa
docs/                 architecture, protocol, hardware, flashing, server, web config, iOS app, test checklist
build/                all build output and caches, git-ignored (build/dist: flashable images)
```

## Build

```bash
nix develop                     # ESP-IDF v5.5.5 + toolchains, Rust (first run downloads ~1 GB)
scripts/sw build                # or: build watch|companion|server
scripts/sw test                 # C++ (ASan/UBSan) and Rust tests + clippy
scripts/sw package              # build/dist/{watch,companion}: images, offsets, merged image
scripts/sw ios-sdk [Xcode.xip]  # once: extract iPhoneOS.sdk from an Xcode archive
scripts/sw ipa                  # build/dist/ios/SWCompanion.ipa
```

`scripts/sw` re-enters `nix develop` automatically when run from a plain shell.

## Flash

```bash
scripts/sw flash watch /dev/ttyACM0 --monitor
scripts/sw flash companion /dev/ttyUSB0 --monitor
```

Exact esptool commands/offsets and NixOS serial permissions: [docs/flashing.md](docs/flashing.md).

### From Windows or macOS

Copy `build/dist/` to the other machine and flash with [espflash](https://github.com/esp-rs/espflash)
(a single binary from its releases page; no Python or ESP-IDF needed). Ports are `COM3`-style
on Windows and `/dev/cu.usbmodem*` (watch) / `/dev/cu.usbserial-*` (companion) on macOS.

```bash
# first flash of a blank board: one merged image at 0x0 (also clears pairings and settings)
espflash write-bin -p COM3 0x0 dist/watch/watch-merged.bin
espflash write-bin -p COM4 0x0 dist/companion/companion-merged.bin

# updates: app only, keeps Bluetooth pairings and companion settings
espflash write-bin -p COM3 0x20000 dist/watch/smartwatch.bin
espflash write-bin -p COM4 0x20000 dist/companion/companion.bin

espflash monitor -p COM4          # serial console (Ctrl+R reset, Ctrl+C quit)
```

## First use

1. Deploy the helper ([docs/server.md](docs/server.md)).
2. Boot the companion; the BLE pairing passkey is fixed to `616616` for the prototype (TODO(PROTOTYPE)); it is also printed on the serial console.
3. Watch → Apps → Settings → Companion → Find & pair → enter `616616`.
4. Watch → Settings → Companion → Server: helper URL/token, ntfy URL/token, Navidrome; Settings →
   Companion → Home Wi-Fi.
5. Watch → Settings → Companion → AirPods → Search (AirPods in pairing mode) → select.
6. Optional: edit the companion's settings from a browser: Watch → Settings → Companion → Web configuration
   ([docs/web-config.md](docs/web-config.md)).

Instead of the companion, an iPhone can provide internet, location and music: import
`SWCompanion.ipa` into LiveContainer, open it and type the code the watch shows
([docs/ios-app.md](docs/ios-app.md)). A connected phone takes priority over the companion.

Then follow [docs/hardware-test-checklist.md](docs/hardware-test-checklist.md).

## Status

Everything compiles and all features work (maps, music, weather, steps) on the Waveshare and Freenove devkits.
Both companion modes have been tested: iOS app and Freenove ESP32. iOS app has some issues regarding music volume:
there isn't really an API to set phone volume directly, so the app does some tricks with volume boosting etc. and it's not perfect.

Some prototype substitutions are marked in code with `TODO(PROTOTYPE)`, `TODO(HARDWARE)`,
`TODO(LTE)` and `TODO(PHONE)` (`grep -rn "TODO(" firmware shared server web`).
