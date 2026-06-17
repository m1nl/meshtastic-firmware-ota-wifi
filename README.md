## WiFi OTA Firmware

 - Fits into 640KB for all esp32 targets
 - Exposes a simple HTTP user interface, which enables user to upload image to be flashed

### Usage

Build the firmware using ESP-IDF 5.5.x
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
After rebooting the device into OTA firmware (the main firmware has to have some way of doing that), open IP address in the browser and use upload form to flash firmware.

Given the changes in `meshtastic-cli`, one has to now use the following command to make the node boot into OTA mode:
```
meshtastic --host <IP / host> --ota-update <whatever_file_does_not_matter>
```
The above command will try to send the update over TCP, but simply stop it and you should be able to open a web page allowing you to upload your firmware file.

### Details

 - To be able to connect to a WiFi access point, OTA firmware expects to find `ssid` and `psk` string fields in the NVRAM storage under `ota-wifi` namespace.
 - After successful flashing, a boolean field `updated` is raised, so that the main firmware can handle the "first boot after update" scenario.
