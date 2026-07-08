# XIAO ESP32-C6 Zigbee Water Meter

This project implements a Zigbee water meter for the Seeed Studio XIAO ESP32-C6 using ESP-IDF 5.4.1 and Espressif Zigbee SDK 1.6.x. It counts water meter pulses, stores the total in NVS, reports Metering and Battery attributes, and supports Zigbee OTA firmware updates through Zigbee2MQTT.

## Hardware

| Signal | XIAO ESP32-C6 |
| :--- | :--- |
| Water meter pulse | GPIO1 |
| Battery divider ADC | A0 / D0 / GPIO0 |
| GND | GND |
| Sensor VCC, if required | 3.3V or 5V, depending on the sensor |

The pulse input enables the internal pull-up, so a reed switch or open-drain pulse output between GPIO1 and GND works. GPIO1 is used for both light sleep and deep sleep wakeup. The previous GPIO22 wiring is not suitable for deep sleep wakeup on ESP32-C6. The battery ADC is calibrated from the stable supply measurement where `raw_avg=2515` equals 2.37V at A0/D0/GPIO0. The battery divider conversion is calibrated from the 3xAAA measurement where 4.83V battery voltage produces 2.27V at A0/D0/GPIO0.

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

## Battery Sleep Measurement

The firmware supports three build-time sleep modes through `idf.py menuconfig` under `Watermeter`:

| Mode | Behavior |
| :--- | :--- |
| `off` | No sleep, useful for debugging. |
| `light` | Default. RAM, FreeRTOS tasks, and Zigbee context stay alive. The device sleeps only when the Zigbee stack emits `ESP_ZB_COMMON_SIGNAL_CAN_SLEEP`. |
| `deep` | Battery-first mode. CPU/RAM/Zigbee stack are powered down; GPIO1 or a 10 minute timer wakes the chip and `app_main` starts again. |

Both sleep modes use GPIO1 for pulse wakeup. The common safety rules are: do not sleep before Zigbee join/restore, do not sleep during OTA, do not sleep while GPIO1 is held low, save the pulse count to NVS before sleeping, and keep Zigbee awake after a pulse report. Light sleep uses a 3 second awake window. Deep sleep uses an 8 second awake window because reports must be queued after a cold boot.

The sleepy Zigbee configuration is:

| Setting | Value |
| :--- | :--- |
| `rx_on_when_idle` | `false` |
| End device aging timeout | 64 minutes |
| Keepalive | 3000 ms |
| Periodic report / deep timer wake | 10 minutes |
| Poll Control check-in | 10 minutes |
| Poll Control long poll | 5 seconds |
| Poll Control short poll | 0.5 seconds |
| Poll Control fast poll timeout | 10 seconds |

Deep sleep can delay incoming coordinator commands and makes OTA less convenient because the device is offline between wakeups. Use `light` or `off` when actively testing OTA.

For a quick before/after check with a KWS-2301C USB-C power meter:

1. Connect power source -> KWS-2301C -> USB-C cable -> XIAO ESP32-C6.
2. Use the same cable, power source, Zigbee network, and report interval for both measurements.
3. Wait until the device has joined or restored the Zigbee network.
4. For light sleep, watch the log for `Zigbee stack can sleep for ... ms`, `Returned from Zigbee sleep`, and `Wakeup check`.
5. For deep sleep, watch the log for `Entering deep sleep`, a new boot log, and the `boot wake cause`.
6. Compare the idle current before and after those sleep logs appear.

The USB meter is useful for proving a visible drop from the old roughly 50 mA idle baseline, but it is not a true battery-life measurement. The XIAO board's USB path, regulator, power-management/charging circuit, LEDs, USB serial/JTAG, logging, and Zigbee polling all add current that a bare ESP32-C6 sleep number does not include. For final battery validation, measure current in series with the battery or board supply path, not only through USB.

If the USB meter never drops below the old idle value, check the serial log first. A healthy light-sleep cycle should show repeated Zigbee `CAN_SLEEP` messages and wake causes. A healthy deep-sleep cycle should show periodic cold boots from timer or GPIO wakeup. If GPIO1 is held low, the firmware intentionally keeps Zigbee awake until the pulse input returns high to avoid wake loops and duplicate pulse counts.
