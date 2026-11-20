# iOS companion app

`phone/ios` is an iPhone app that replaces the ESP32 companion and also gives the watch GPS. It
is built on Linux and meant to run inside [LiveContainer](https://github.com/LiveContainer/LiveContainer).

**Status: compiled only. It has not run on an iPhone, in LiveContainer or against the watch yet.**

## Building

```sh
scripts/sw ios-sdk [path/to/Xcode.xip]   # once; default ~/Downloads/Xcode_27.2.xip
scripts/sw ipa                           # -> build/dist/ios/SWCompanion.ipa
```

No Mac or Swift toolchain: Objective-C++ compiled with clang and linked with lld against the SDK
from the Xcode archive. `scripts/extract_ios_sdk.py` handles two things about that SDK:

* Files shared between SDKs are hardlinks in the archive. One entry per inode holds the data, the
  others hold `NULLcanary` filler, so a plain `cpio` extraction leaves broken headers.
* Library stubs list `arm64e` targets only, some as `arm64e.x1`, which LLVM rejects. They are
  rewritten to `arm64`.

The app is pseudo-signed with `ldid`; LiveContainer signs it on import. The linker leaves extra
header space because LiveContainer rewrites load commands to load the app as a library.

## Link to the watch

iOS doesn't allow a BLE peripheral with custom services to be reliably found in the background, and
LiveContainer's host app doesn't declare `bluetooth-peripheral` anyway. So the roles swap:

* The watch advertises service `7a1e0010-...` with the usual RX/TX/Info characteristics.
* The phone connects (a pending connection, completed by iOS whenever the watch is in range) and
  subscribes to TX. The watch shows a random 6-digit code; iOS asks for it. Unauthenticated links
  are dropped.
* Then SWP as usual: the watch sends Hello, the phone answers HelloAck with `Role::Phone`.

A connected phone takes priority: the watch drops the companion link and doesn't retry it while
the phone stays connected. The ESP32 companion path still works as before without a phone.

## What differs from the ESP32 companion

| Area | Phone |
|------|-------|
| Position | CoreLocation, source `Phone`, sent at most every 2 s |
| Songs and memos | `Documents/music`, `Documents/memos`, excluded from backups; `library.json` format unchanged |
| Tile cache | `Library/Caches/maps` (iOS may purge it), 4 GB, least recently used dropped |
| Music sync | Any network (Wi-Fi, cellular, Tailscale), the whole library up to 256 MB of free space left, 3 parallel downloads, continues during playback (the queue plays a snapshot) |
| Area prefetch | Any network |
| Audio output | Whatever iOS routes to. The watch can't pick headphones: `AudioConnect` etc. only report the route |
| Volume | The slider is the music's loudness: the player attenuates when the system volume is higher. When lower it tries to raise it through a hidden `MPVolumeView` slider (may not work in the background) and boosts the rest, up to +12 dB through a peak limiter. Volume buttons during playback move it by the same step |
| Memos | The current input: headphones' microphone over Bluetooth if connected, else the phone's. 16 kHz WAV |
| Wi-Fi settings, LAN settings page | Not supported; settings are in the app |
| Secrets | `Library/Application Support/secrets.plist`: file-protected, excluded from backups. Navidrome may use plain http on a private or Tailscale (100.64/10) IP |

## Navidrome behind a proxy

iOS can block plain `http://` inside LiveContainer (App Transport Security comes from the host app).
Use an `https://` URL. "Navidrome auth header" is sent as the `auth` header, as typed, on every
request, like ntfy's; a redirect to another host is refused while it is set.

## Background running

iOS honours the Info.plist of LiveContainer's host app: `audio`, `bluetooth-central` and `location`
are declared there. Background location updates keep the process alive. Grant location "Always"
when iOS offers it. "Silent audio keep-alive", on by default, does the same job through the audio
mode. iOS can still terminate the app, and LiveContainer can't relaunch a guest
on its own: reopen it if the watch's LINK goes red.
