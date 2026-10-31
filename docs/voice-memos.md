# Voice memos (deferred)

Not implemented in this release (`TODO(PROTOTYPE)` in `firmware/watch/main/ui/screens/apps.cpp`).
The production watch has no microphone, so the plan is:

1. Companion adds the Bluetooth **HFP Audio Gateway** role next to A2DP source
   (`CONFIG_BT_HFP_AG_ENABLE`), opening an SCO link to the AirPods microphone on demand.
2. Watch Memos app sends `MemoStart/MemoStop` SWP commands; the companion records
   mSBC/CVSD (16/8 kHz) and encodes to AAC with `esp_audio_codec` into `/sdcard/memos/`
   (256 MB quota already reserved in `ccore/quota.hpp`).
3. While recording, A2DP playback is paused (AirPods cannot do HFP + A2DP at once).
4. Optional upload to the VPS helper for transcription.

Flash budget: HFP AG adds roughly 60–100 KB to the companion image (currently 1.9 MB of 3.9 MB).
