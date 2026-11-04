# Voice memos

Status: recording verified on AirPods (stop from the watch and from the AirPods). Playback after a recording is still being verified (TODO(HARDWARE)).

The watch has no microphone. Memos are recorded through the headphones' microphone:

1. The watch Memos app sends `MemoCommand{Start}` over SWP.
2. The companion pauses music if it's playing (AirPods can't stream A2DP and run a hands-free
   audio link at the same time). It then opens a Bluetooth **HFP Audio Gateway** service-level
   connection to the remembered A2DP headphones and turns on voice recognition (`+BVRA:1`), which
   tells the headphones to route their microphone to us. Then it opens the SCO audio link.
3. The Bluetooth controller sends SCO over HCI, and Bluedroid decodes mSBC (16 kHz, wide-band)
   or CVSD (8 kHz) to 16-bit mono PCM.
4. The PCM goes through a 64 KB PSRAM stream buffer to a writer task, which writes
   `/sdcard/memos/YYYYMMDD-HHMMSS.wav`. The WAV header is patched with the length at the end.
5. Stop (from the watch, the headphones ending voice recognition, the audio link dropping, or
   the 30-minute cap) closes the file, then closes every profile so the Bluetooth connection itself
   goes down. On AirPods, A2DP sent over a connection that has carried the hands-free audio link
   stays congested (`l2cab is_cong_cback_context` floods the log; playback stutters or is silent)
   until the connection is recreated, so playback afterwards reconnects fresh. The companion then
   sends `MemoStatus` and `MemoList` to the watch.

Playback goes through the normal music player as the virtual playlist `memos` (newest first). The
WAV decoder output is resampled to the A2DP 44.1 kHz stereo stream.

Limits: 40 memos are listed, and storage stops at the memos quota in `ccore/quota.hpp`. If the
headphones don't open the audio link within 12 s, recording fails with "Headphones not
available".

Config: companion `sdkconfig.defaults` enables `BT_HFP_AG_ENABLE`, `BT_HFP_AUDIO_DATA_PATH_HCI`,
`BT_HFP_WBS_ENABLE` and `BTDM_CTRL_BR_EDR_SCO_DATA_PATH_HCI` with one sync connection. To make
room in IRAM, the ring buffer and FreeRTOS task-level functions are placed in flash.

Not done: transcription, renaming, and sharing memos to the phone.
