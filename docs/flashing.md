# Flashing

## NixOS serial access

Both boards enumerate as USB serial devices:

| Board | Device | Chip |
|-------|--------|------|
| Watch (ESP32-S3 native USB) | `/dev/ttyACM0` | USB-Serial/JTAG |
| Companion (Freenove) | `/dev/ttyUSB0` | CH340 |

Your user needs the `dialout` group (NixOS module config, no imperative changes):

```nix
users.users.<you>.extraGroups = [ "dialout" ];
```

Then `sudo nixos-rebuild switch` and log out/in. The CH340 driver (`ch341`) is in the default
kernel. If ModemManager is enabled it may grab `/dev/ttyACM*` briefly; disable it or add
`services.udev.extraRules = ''ATTRS{idVendor}=="303a", ENV{ID_MM_DEVICE_IGNORE}="1"'';`.

## Quick path

```bash
scripts/sw build watch && scripts/sw build companion
scripts/sw package                     # -> build/dist/watch, build/dist/companion
scripts/sw flash watch /dev/ttyACM0 --monitor
scripts/sw flash companion /dev/ttyUSB0 --monitor
```

## Manual esptool commands (blank devices)

Watch (ESP32-S3, 16 MB layout):

```bash
cd build/dist/watch
esptool.py --chip esp32s3 --port /dev/ttyACM0 --baud 460800 write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 bootloader.bin 0x8000 partition-table.bin 0xf000 ota_data_initial.bin 0x20000 smartwatch.bin
# or the single merged image:
esptool.py --chip esp32s3 --port /dev/ttyACM0 write_flash 0x0 watch-merged.bin
```

Companion (ESP32, 4 MB):

```bash
cd build/dist/companion
esptool.py --chip esp32 --port /dev/ttyUSB0 --baud 460800 write_flash \
  --flash_mode dio --flash_freq 80m --flash_size 4MB \
  0x1000 bootloader.bin 0x8000 partition-table.bin 0x20000 companion.bin
# or: esptool.py --chip esp32 --port /dev/ttyUSB0 write_flash 0x0 companion-merged.bin
```

`dio` in the flash arguments is expected for the watch: the second-stage bootloader
switches to QIO as configured. Offsets come from ESP-IDF's `flasher_args.json`;
`build/dist/*/flash-manifest.json` lists them with SHA-256 checksums, `flash-args.txt` the
esptool arguments, and `<target>-merged.bin` is a single image to write at 0x0 on a blank
device (it also blanks the gaps, i.e. Bluetooth pairings and companion settings).

Partition maps: `firmware/watch/partitions.csv` (nvs, otadata, ota_0/ota_1 6 MB each,
nvs_user 1 MB) and `firmware/companion/partitions.csv` (nvs, phy, `cfg` 64 KB at 0x10000,
factory app 3.875 MB; no OTA because two app slots do not fit in 4 MB).

## Companion settings

Flashing never touches the companion's `cfg` partition (Wi-Fi, helper, ntfy and Navidrome
settings). Set them on the watch (Settings → Server, Settings → Connections → Home Wi-Fi)
or in the web app.

`esptool.py erase_flash` wipes everything including bonds and `cfg`.
