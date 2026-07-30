# XIAO ESP32-C6 Zigbee Water Meter

This project implements a Zigbee water meter for the Seeed Studio XIAO ESP32-C6 using ESP-IDF 5.4.1
and Espressif Zigbee SDK 1.6.x. It counts water meter pulses, stores every pulse in NVS, reports
Metering and Battery attributes, and supports Zigbee OTA updates through Zigbee2MQTT.

## Hardware

The production hardware assumed by the default configuration is:

| Signal | Connection |
| :--- | :--- |
| LiPo battery | XIAO `BAT` and `GND` |
| DRV5032FBLPG VCC | XIAO `3V3` |
| DRV5032FBLPG GND | XIAO `GND` |
| DRV5032FBLPG OUT | GPIO1 |
| Battery divider | `BAT -> 200 kOhm -> GPIO0 -> 200 kOhm -> GND` |
| Zigbee antenna | External antenna on the U.FL connector |

Place a 100 nF capacitor between the Hall sensor VCC and GND, close to the sensor. The
DRV5032FBLPG output is push-pull, so GPIO1 has no firmware pull-up or external pull-up. A falling
edge is one 10-liter pulse; a rising edge only rearms the input. Holding the output low counts one
pulse, not repeated pulses.

Place a 100 nF capacitor between GPIO0 and GND, close to the XIAO. The 200 kOhm / 200 kOhm divider
draws about 10.5 uA from a 4.2 V battery. The ADC uses the ESP32-C6 eFuse curve-fitting calibration;
there is no board-specific raw ADC calibration constant. Divider resistance and the 0%/100%
voltage limits are build-time settings.

The firmware explicitly selects the external antenna on every normal boot:

- GPIO3 is driven low to enable the XIAO RF switch.
- GPIO14 is driven high to select the U.FL path.
- Both pins are floated before deep sleep to avoid holding the RF switch active.

This build is not intended for the onboard ceramic antenna. A missing or poor external antenna can
cause repeated join/report attempts and therefore much higher energy consumption.

The XIAO ESP32-C6 has 4 MB flash. This project uses two `0x1E0000` OTA application slots plus
Zigbee storage near the end of flash.

## Build

The normal build uses the battery-oriented production defaults:

```bash
. $HOME/.espressif/tools/activate_idf_v5.4.1.sh
idf.py set-target esp32c6
idf.py build
```

`sdkconfig.defaults` is the production source of truth. The generated `sdkconfig` is not a defaults
file. To inspect or tune the Watermeter settings:

```bash
idf.py menuconfig
```

For a separate diagnostic build with INFO logs and USB Serial/JTAG console:

```bash
idf.py -B build-diagnostic \
    -DSDKCONFIG="$PWD/sdkconfig.diagnostic" \
    -DSDKCONFIG_DEFAULTS="$PWD/sdkconfig.defaults;$PWD/sdkconfig.diagnostic.defaults" \
    build
```

Do not use the diagnostic image for battery-life measurements. Its console and additional logging
change both awake time and board current.

`OTA_FILE_VERSION` controls the Zigbee OTA file version compiled into the firmware. It must only
increase:

```bash
OTA_FILE_VERSION=2 idf.py build
```

Run the pure host tests without an ESP32:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

## Sleep Policy

The production profile uses a hybrid policy. "Deep sleep mode" does not mean that the device
immediately deep-sleeps after every CPU operation:

- While the application and Zigbee stack are active, ESP-IDF dynamic frequency scaling uses
  40-160 MHz and automatic light sleep.
- Deep sleep is used only after required reports are confirmed or an awake window expires.
- A short session can therefore process several nearby pulses without paying a full cold-boot and
  Zigbee restore cost for every pulse.

The normal state sequence is:

1. On an ordinary power-on or reset, GPIO1 already being low is not counted as a pulse.
2. When idle, the device deep-sleeps waiting for either GPIO1 to fall or the retained report timer.
3. A GPIO1 falling wake counts exactly one pulse and immediately commits the new total to NVS.
4. Zigbee sends `CurrentSummationDelivered` and custom scaled summation attribute `0xFC00`
   sequentially. Both send-status callbacks must succeed within 10 seconds.
5. If GPIO1 is high after delivery, the device keeps a 30-second activity window. CPU idle time
   inside this window is light sleep. Every new pulse restarts the 30-second window.
6. If GPIO1 remains low after the pulse, the device does not wait for release while awake. It enters
   deep sleep armed for GPIO1 high.
7. The release wake only rearms GPIO1 low and returns directly to deep sleep. It does not start NVS,
   Zigbee, or the full application and does not count a pulse.
8. A scheduled wake reports the meter and returns to deep sleep without the 30-second pulse grace.

If either mandatory meter report cannot be confirmed, the device stops trying after the 10-second
delivery window. It deep-sleeps and retries after 5, 15, 30, then 55 minutes. A successful delivery
resets this backoff. This limits energy spent when the coordinator, parent, or RF path is unavailable.

Battery attributes are attempted on the first ordinary boot and then every 6 hours. Their send status
does not hold the device awake and does not block a meter report. Meter data has higher priority than
battery reporting and OTA.

Factory-new commissioning is allowed up to 2 minutes on the first boot. Later attempts use
10-second windows separated by the same 5/15/30/55-minute backoff. An already joined device does not
repeat network steering. OTA uses a 500 ms parent poll while a transfer is active and blocks deep
sleep until the transfer finishes.

### Current Production Values

All values below are defined in `main/Kconfig.projbuild` and selected by `sdkconfig.defaults`.

| Setting | Default | Purpose |
| :--- | ---: | :--- |
| Sleep mode | deep | Hybrid light sleep during sessions, deep sleep between sessions |
| HP CPU range | 40-160 MHz | Dynamic frequency scaling while awake |
| Meter/report timer | 55 min | Maximum normal interval between meter reports |
| Battery report timer | 6 h | Battery ADC and Battery cluster report interval |
| Pulse activity grace | 30 s | Reused session window after the last pulse |
| Mandatory delivery window | 10 s | Maximum awake time spent confirming meter attributes |
| Non-critical report flush | 500 ms | Optional battery report transmit opportunity |
| Parent poll during delivery | 1 s | Faster parent contact while sending meter data |
| Parent poll during OTA | 500 ms | OTA transfer responsiveness |
| Normal keep-alive / long poll | 55 min | Low-traffic sleepy-end-device interval |
| End-device aging timeout | 128 min | Parent retention window |
| Commissioning window | 2 min | First factory-new join window |
| Retry delays | 5/15/30/55 min | Report and commissioning backoff |
| IEEE 802.15.4 TX power | 20 dBm | External-antenna link budget |
| Battery divider | 200/200 kOhm | GPIO0 voltage divider |
| Battery 0% / 100% | 3300/4200 mV | Linear reported percentage endpoints |
| ADC samples | 16 | Averaged calibrated battery samples |

The Battery percentage is intentionally a simple linear estimate between the configured endpoints.
It is useful for trends and low-battery automation, but it is not a precise LiPo state-of-charge
model.

### Sleep Modes

`idf.py menuconfig -> Watermeter -> Sleep mode` also exposes:

| Mode | Behavior |
| :--- | :--- |
| `off` | Radio stays on when idle; intended only for debugging. |
| `light` | RAM and Zigbee context remain powered; no application-controlled deep sleep. |
| `deep` | Production hybrid policy described above. |

Deep sleep intentionally delays coordinator-initiated commands while the device is offline. With
the default policy, such a command may wait until the next pulse or scheduled wake, up to about
55 minutes. Pulse accounting and outbound meter delivery take priority over immediate inbound
control and OTA convenience.

## First Flash and Migration

The move from the old single-app 2 MB layout to the 4 MB OTA layout requires one USB flash because
the partition table changes:

```bash
idf.py -p /dev/cu.usbmodem11101 flash monitor
```

The water counter NVS partition remains at `0x9000`, so the pulse count is preserved. Zigbee storage
moves from the old offset to `0x3E0000`, so the first migration requires re-pairing or a
backup/restore of Zigbee storage.

You can confirm the real flash size before migration with:

```bash
esptool.py --chip esp32c6 flash_id
```

## Zigbee OTA

The firmware exposes OTA Upgrade client cluster `0x0019` on endpoint `1` with:

| Field | Value |
| :--- | :--- |
| OTA magic | `0x0BEEF11E` |
| Manufacturer code | `0x131B` |
| Image type | `0x0001` |
| File version | `OTA_FILE_VERSION` |

GitHub Actions builds with ESP-IDF 5.4.1, checks that `watermeter.bin` fits in one `0x1E0000` slot,
creates a Zigbee `.ota` file, writes `ota/index.json`, and publishes both through GitHub Pages on
pushes to `main`.

Configure Zigbee2MQTT:

```yaml
ota:
  zigbee_ota_override_index_location: https://<user>.github.io/zigbee-watermeter/ota/index.json
```

Do not offer the raw `watermeter.bin` to Zigbee2MQTT; use the generated `.ota` file.

The device uses rollback-enabled OTA slots. After an OTA boot succeeds, the application marks the
new image valid early in `app_main`. Zigbee sleep is disabled during an OTA transfer so the device
does not sleep in the middle of the update.

## Power Validation

Battery voltage measured every few hours can reveal a severe problem, but it cannot separate sleep
current, radio retry current, regulator losses, or LiPo discharge-curve effects. For engineering
validation, measure current in series with the battery or board supply:

1. Verify idle deep-sleep current with the Hall output high and the Zigbee network available.
2. Hold the Hall output low and verify that only one pulse is counted and current returns to the
   deep-sleep level while waiting for release.
3. Disconnect or turn off the coordinator and verify that each active attempt is limited to about
   10 seconds, followed by the configured retry sleeps.
4. Generate several pulses less than 30 seconds apart and compare the session energy against
   separate cold boots.
5. Repeat with the external antenna in the final enclosure and installation position.

A USB power meter includes the XIAO USB, charging, and console paths and usually cannot resolve
short radio bursts or deep-sleep current accurately. Use the production build and measure through
the battery path for the final result. Firmware alone cannot guarantee a battery-life number without
measured sleep current, awake current, join/report success rate, and real pulse frequency.
