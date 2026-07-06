# XIAO ESP32-C6 Zigbee Water Meter

This project implements a Zigbee water meter for the Seeed Studio XIAO ESP32-C6 using ESP-IDF 5.4.1 and Espressif Zigbee SDK 1.6.x. It counts water meter pulses, stores the total in NVS, reports Metering and Battery attributes, and supports Zigbee OTA firmware updates through Zigbee2MQTT.

## Hardware

| Signal | XIAO ESP32-C6 |
| :--- | :--- |
| Water meter pulse | GPIO22 |
| Battery divider ADC | A0 / D0 / GPIO0 |
| GND | GND |
| Sensor VCC, if required | 3.3V or 5V, depending on the sensor |

The pulse input enables the internal pull-up, so a reed switch or open-drain pulse output between GPIO22 and GND works. The battery input assumes a 2:1 divider into GPIO0.

The XIAO ESP32-C6 has 4MB flash. This project uses two 0x1E0000 OTA app slots plus Zigbee storage near the end of flash.

## Build

```bash
. $HOME/.espressif/tools/activate_idf_v5.4.1.sh
idf.py set-target esp32c6
idf.py build
```

`OTA_FILE_VERSION` controls the Zigbee OTA file version compiled into the firmware. It must only increase:

```bash
OTA_FILE_VERSION=2 idf.py build
```

## First Flash and Migration

The move from the old single-app 2MB layout to the 4MB OTA layout requires one USB flash because the partition table changes:

```bash
idf.py -p /dev/cu.usbmodem11101 flash monitor
```

The water counter NVS partition remains at `0x9000`, so the pulse count is preserved. Zigbee storage moves from the old offset to `0x3E0000`, so the first migration requires re-pairing or a backup/restore of Zigbee storage.

You can confirm real flash size before migration with:

```bash
esptool.py --chip esp32c6 flash_id
```

## Zigbee OTA

The firmware exposes OTA Upgrade client cluster `0x0019` on endpoint `1` with:

| Field | Value |
| :--- | :--- |
| Manufacturer code | `0x131B` |
| Image type | `0x0001` |
| File version | `OTA_FILE_VERSION` |

GitHub Actions builds with ESP-IDF 5.4.1, checks that `watermeter.bin` fits in one `0x1E0000` slot, creates a Zigbee `.ota` file, writes `ota/index.json`, and publishes both through GitHub Pages on pushes to `main`.

Configure Zigbee2MQTT:

```yaml
ota:
  zigbee_ota_override_index_location: https://<user>.github.io/zigbee-watermeter/ota/index.json
```

Do not offer the raw `watermeter.bin` to Zigbee2MQTT; use the generated `.ota` file.

## Notes

The device uses rollback-enabled OTA slots. After an OTA boot succeeds, the application marks the new image valid early in `app_main`. Zigbee sleep is disabled during an OTA transfer so an end device does not sleep in the middle of the update.
