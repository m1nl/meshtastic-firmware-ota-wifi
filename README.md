## WiFi OTA Firmware

 - Fits into 640KB for all esp32 targets
 - Compatible with `espota.py` from arduino-esp32 and customized version (in `contrib` directory) with support for coredumps

### Usage

Build the firmware using ESP-IDF 5.4.x
```
source ~/dev/esp-idf/export.sh
idf.py build
```
and write OTA binary to the 2nd OTA partition (take its address from partition table entry for `flashApp` or `ota_1` for your platform):
- default partition table defines `flashApp` at 0x260000
- for targets with 8MB flash (incl. most Heltecs with ESP32S3) it's 0x340000 (`ota_1`)
```
esptool.py --port <...> write_flash 0x340000 build/OTA-WiFi.bin
```
After rebooting the device into OTA firmware (the main firmware has to have some way of doing that), upload a new main firmware binary over network:
```
espota.py --ip <device-ip-address> --file <new-main-firmware.bin> flash
```
Uploading a filesystem image to a `spiffs` partition:
```
espota.py --ip <device-ip-address> --file <littlefs.bin> spiffs
```
Downloading a coredump from `coredump` partition
```
espota.py --ip <device-ip-address> --file coredump.bin coredump
```

### Details

 - To be able to connect to a WiFi access point, OTA firmware expects to find `ssid` and `psk` string fields in the NVRAM storage under `ota-wifi` namespace.
 - After successful flashing, a boolean field `updated` is raised, so that the main firmware can handle the "first boot after update" scenario.
 - OTA firmware waits for an upload only for some limited time and then reboots back to main on timeout.
