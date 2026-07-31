# XIAO ESP32-C6 Zigbee Water Meter

Battery-powered Zigbee water meter firmware for the Seeed Studio XIAO ESP32-C6. It counts Hall-sensor pulses, commits every accepted pulse to NVS, reports standard and custom ZCL attributes to Zigbee2MQTT, supports Zigbee OTA, and uses a hybrid light/deep-sleep policy.

The firmware is C17 on ESP-IDF 5.4.1 with Espressif Zigbee SDK 1.6.x. It is not an Arduino or PlatformIO project.

## Reference Hardware

The table describes the exact tested assembly. Compatible substitutions require their own electrical and power validation.

| Item | Exact reference | Relevant specification | Source |
| :--- | :--- | :--- | :--- |
| MCU board | Seeed Studio XIAO ESP32-C6, ESP32-C6FH4 | 4 MB flash, 512 KB SRAM, native IEEE 802.15.4, 21 x 17.8 mm | [Seeed product guide](https://wiki.seeedstudio.com/xiao_esp32c6_getting_started/), [schematic](https://files.seeedstudio.com/wiki/SeeedStudio-XIAO-ESP32C6/XIAO-ESP32-C6_v1.0_SCH_PDF_24028.pdf) |
| Battery | Marking `YJ781139/2C` | LiPo, 3.7 V nominal, 320 mAh, 1.184 Wh | Cell marking; protection circuit and thresholds are not documented |
| Hall sensor | Texas Instruments `DRV5032FBLPG` | 5 Hz, typically 0.54 uA, omnipolar, push-pull, TO-92/LPG, 1.65-5.5 V | [TI part page](https://www.ti.com/product/DRV5032/part-details/DRV5032FBLPG), [datasheet](https://www.ti.com/lit/ds/symlink/drv5032.pdf) |
| Sensor bypass capacitor | Suntan `008WL2A104KSBB5081R` | 100 nF, 100 V, X7R, +/-10%, through-hole, 5.08 mm pitch | [Comet order code 5774](https://store.comet.bg/en/Catalogue/Product/5774/) |
| Battery divider | `2 x 200 kOhm` resistors | 1:2 divider; exact manufacturer, tolerance, and power rating are unknown | As-built assembly |
| Zigbee antenna | Quectel `YF0023AA` | Adhesive FPC, U.FL, 2.4-2.5 GHz band, 5.0 dBi listed gain, 23 x 12 mm, 100 mm cable | [Comet order code 46832](https://store.comet.bg/en/Catalogue/Product/46832/) |
| Water meter | Maddalena `SJM20.0I0M` as marked | DN20, 130 mm, R100, T50, Q3 4.0 m3/h, `P=10 L` | Unit nameplate; [Maddalena single-jet family](https://www.maddalena.it/en/mecto-sj-plus-sj-evo-water-meter-non-stop-innovation/) |

The LiPo wires are soldered directly to the XIAO `BAT+` and `BAT-` pads. The XIAO onboard charger charges the 3.7 V cell from USB-C. Verify polarity before connecting the battery. Do not connect the cell to `3V3` or `5V`.

## Wiring

```text
LiPo positive  ------------------------------------------ XIAO BAT+
LiPo negative  ------------------------------------------ XIAO BAT- / GND

XIAO BAT+ ---- 200 kOhm ----+---- XIAO A0 / D0 / GPIO0
                             |
                          200 kOhm
                             |
XIAO GND --------------------+

XIAO 3V3 ----------------------------------------------- DRV5032 VCC
XIAO GND ----------------------------------------------- DRV5032 GND
XIAO D1 / GPIO1 <--------------------------------------- DRV5032 OUT

                       100 nF
DRV5032 VCC -------------||----------------------------- DRV5032 GND
                  mount close to sensor

XIAO U.FL ------------------------------------------------ Quectel YF0023AA
```

There is no capacitor on `GPIO0/A0` in the tested battery-divider assembly. The only added capacitor is the 100 nF sensor bypass capacitor between DRV5032 VCC and GND.

The DRV5032 output is push-pull, so GPIO1 has no firmware or external pull-up. A falling edge is one 10 L pulse; a rising edge rearms the input. Holding the output low counts one pulse and then sleeps while waiting for release. An ordinary power-on/reset with the output already low does not count a pulse.

The 200 kOhm/200 kOhm divider draws about 10.5 uA at 4.2 V. GPIO0 uses ESP32-C6 eFuse curve-fitting ADC calibration and 16 averaged samples. Battery percentage is a linear trend indicator from 3300 mV (0%) to 4200 mV (100%), not a precise LiPo state-of-charge model.

## External Antenna

The production assembly uses only the external U.FL antenna. On every full application boot:

- GPIO3 is driven low to enable control of the XIAO RF switch.
- GPIO14 is driven high to select the external U.FL path.
- Both pins are floated immediately before deep sleep to avoid holding the RF switch active.

A missing, damaged, or poorly placed antenna can cause repeated joins or report retries and dominate energy consumption. Test the link with the final enclosure and installation position.

## Firmware Architecture

| Module | Responsibility |
| :--- | :--- |
| `main/watermeter.c` | Application bootstrap and ESP-IDF platform initialization |
| `main/meter_state.c` | NVS-backed pulse count and scale settings |
| `main/sensor.c` | GPIO ISR, debounce, pulse task, and per-pulse persistence |
| `main/battery.c` | ADC lifecycle and Battery cluster updates |
| `main/battery_math.c` | Pure ADC/divider/percentage calculations |
| `main/meter_math.c` | Pure summation and ZCL integer conversion helpers |
| `main/power_schedule.c` | Pure report and retry scheduling calculations |
| `main/sleep_control.c` | Zigbee sleep, light-sleep sessions, deep-sleep wake/release state |
| `main/xiao_board.c` | XIAO external antenna control |
| `main/zigbee_clusters.c` | Per-cluster builders and endpoint construction |
| `main/zigbee_app.c` | Zigbee lifecycle, commissioning, reporting, and callbacks |
| `main/ota.c` | OTA validation, writing, finalization, and deferred restart |

## Stable Zigbee Contract

Endpoint `1` exposes standard Basic, Identify, Metering, Power Configuration, Poll Control, and OTA client clusters. The externally visible meter contract is:

| Value | Contract |
| :--- | :--- |
| `CurrentSummationDelivered` | Raw persistent pulse count |
| Metering `Multiplier` / `Divisor` | Read-only mirrors of the configured scale |
| Custom `0xFC00` | Scaled summation |
| Custom `0xFC01` | Writable positive scale multiplier |
| Custom `0xFC02` | Writable positive scale divisor |
| Battery voltage / percentage | Standard Power Configuration attributes |
| Default scale | `10 / 1`, so one pulse reports 10 L |

Every accepted pulse is committed to NVS immediately. This intentionally prioritizes water accounting across sudden power loss over flash wear.

## Operations Harness

The dependency-free Python harness is the preferred entry point. It discovers ESP-IDF v5.4.1 and serial ports, reads partition addresses from `partitions.csv`, isolates build profiles, and enforces destructive-operation confirmations.

```bash
python3 tools/watermeter_harness.py doctor
python3 tools/watermeter_harness.py ports
python3 tools/watermeter_harness.py test
python3 tools/watermeter_harness.py build
python3 tools/watermeter_harness.py check-size
```

Run `python3 tools/watermeter_harness.py --help` for all commands. Detailed AI/operator workflows live in:

- `.agents/skills/watermeter-firmware/SKILL.md`
- `.agents/skills/watermeter-hardware-test/SKILL.md`
- `.agents/skills/watermeter-z2m/SKILL.md`

## Manual Build

The equivalent production build is:

```bash
. $HOME/.espressif/tools/activate_idf_v5.4.1.sh
idf.py set-target esp32c6
idf.py build
```

`sdkconfig.defaults` is the production source of truth. Generated `sdkconfig` files are not defaults. The build is valid only when `build/watermeter.bin` is strictly smaller than the `0x1E0000` OTA slot parsed from `partitions.csv`.

Run the pure host tests without an ESP32:

```bash
python3 -m unittest discover -s tests/python -p 'test_*.py'
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

## Flash And Logs

Normal flashing builds first and preserves pulse NVS and Zigbee pairing:

```bash
python3 tools/watermeter_harness.py flash --profile production
python3 tools/watermeter_harness.py monitor --profile production
```

The harness auto-selects the port only when exactly one candidate exists. Use `--port` when several boards are connected. XIAO native USB may remain in ROM download mode after flashing; press RESET once without holding BOOT. A disappearing USB port after normal startup is expected when production firmware enters deep sleep.

Do not erase flash to fix a monitor reconnect or ROM download-mode condition.

### Diagnostic Profile

Diagnostic mode has an independent `sdkconfig.diagnostic` and `build-diagnostic`:

```bash
python3 tools/watermeter_harness.py build-diagnostic
python3 tools/watermeter_harness.py flash --profile diagnostic
python3 tools/watermeter_harness.py monitor --profile diagnostic
```

It enables INFO logs and USB Serial/JTAG while preserving NVS and Zigbee storage. It changes awake time and current, so never use it for battery measurements. Restore production firmware after diagnosis.

### Erase Policy

Ordinary flash never erases state. Recovery commands require exact confirmations:

```bash
# Preserves pulse NVS but clears pairing; permit join is required afterward.
python3 tools/watermeter_harness.py erase-zigbee --confirm 'ERASE ZIGBEE'

# Destroys firmware, pulse count, OTA state, and pairing.
python3 tools/watermeter_harness.py erase-all --confirm 'ERASE ALL'
```

The Zigbee partition offset is never copied into documentation or scripts; it is parsed from `partitions.csv` at runtime.

## Sleep Policy

Production uses a hybrid policy. The CPU and Zigbee stack use automatic light sleep while an application session is open, then the device enters deep sleep between sessions.

1. An ordinary power-on/reset does not count GPIO1 already being low.
2. Idle deep sleep waits for GPIO1 or the retained report timer.
3. A falling GPIO wake counts once and commits immediately to NVS.
4. Both mandatory Metering reports must succeed within a 10-second delivery window.
5. If GPIO1 is high, a 30-second activity window reuses the live Zigbee session; every new pulse restarts it.
6. If GPIO1 remains low, the device deep-sleeps waiting for GPIO1 high instead of blocking a task.
7. The release wake rearms GPIO1 low and returns directly to deep sleep without counting or starting Zigbee.
8. A scheduled wake reports the meter and returns to deep sleep without the pulse grace window.

Failed mandatory reports retry after 5, 15, 30, and then 55 minutes. A successful delivery resets the backoff. Factory-new commissioning gets one 2-minute window; later attempts are limited to 10 seconds and use the same backoff. Battery reporting is attempted on first ordinary boot and every 6 hours without blocking meter delivery. OTA temporarily blocks deep sleep and uses a faster parent poll.

### Production Defaults

| Setting | Default |
| :--- | ---: |
| Sleep mode | Hybrid deep/light |
| Active HP CPU | Fixed 160 MHz |
| Sensor debounce | 1 s |
| Normal meter timer | 55 min |
| Battery report timer | 6 h |
| Pulse activity grace | 30 s |
| Mandatory delivery window | 10 s |
| Optional report flush | 500 ms |
| Parent poll during delivery | 1 s |
| Parent poll during OTA | 500 ms |
| Zigbee keep-alive | 55 min |
| End-device aging timeout | 128 min |
| IEEE 802.15.4 TX power | 20 dBm |
| Retry delays | 5/15/30/55 min |

All tunable values are defined in `main/Kconfig.projbuild` and selected in `sdkconfig.defaults`.

## Zigbee2MQTT Converter

The canonical external converter is:

```text
external_converters/zigbeehive-watermeter.mjs
```

The matching device icon is:

```text
external_converters/device_icons/zigbeehive-watermeter.png
```

Zigbee2MQTT 2.11 and newer requires `advanced.enable_external_js: true`. Install the standalone file under the Zigbee2MQTT data directory's `external_converters/` folder. This repository does not own unrelated Tuya or light-sensor converters.

Install the icon under `<zigbee2mqtt-data>/device_icons/` and select it for the device:

```yaml
devices:
  '0xb43a45fffe8a73c4':
    icon: device_icons/zigbeehive-watermeter.png
```

Audit the configured homelab without modifying it:

```bash
python3 tools/watermeter_harness.py z2m-audit
```

Deployment requires the state-bound token printed by a separately reviewed audit plus its typed confirmation. Rollback requires a separate typed confirmation. See `.agents/skills/watermeter-z2m/SKILL.md` before changing the remote host.

## Zigbee OTA

| Header field | Value |
| :--- | :--- |
| Magic | `0x0BEEF11E` |
| Manufacturer code | `0x131B` |
| Image type | `0x0001` |
| File version | Explicit monotonic `OTA_FILE_VERSION` |

Create a local image with an explicit version:

```bash
python3 tools/watermeter_harness.py ota-build --file-version 42
```

GitHub Actions uses `github.run_number`, verifies the binary size, creates the Zigbee `.ota` and `index.json`, and publishes them through GitHub Pages from `main`. Zigbee2MQTT must use the generated index; never offer raw `watermeter.bin` as a Zigbee OTA image.

OTA slots have rollback enabled. A successful boot marks the image valid early in `app_main`. OTA completion schedules a deferred restart instead of restarting inside the Zigbee callback.

## Power Validation

The Hall sensor and divider together account for roughly 11 uA typical before XIAO board losses. A 320 mAh cell discharging in two days therefore indicates substantial additional board current, prolonged awake time, RF retries, charging-path effects, a damaged cell, or another hardware fault; firmware settings alone cannot identify the cause.

Validate in the production profile with USB disconnected:

1. Confirm idle deep-sleep current with GPIO1 high and the coordinator available.
2. Hold the Hall output low and verify one count, no watchdog, and return to deep-sleep current while waiting for release.
3. Turn off the coordinator and verify bounded 10-second attempts followed by configured retry sleeps.
4. Generate pulses less than 30 seconds apart and compare one reused session with repeated cold boots.
5. Test the external antenna in its final placement.
6. Record battery voltage, real pulse activity, outages, and resets over several days.

A multimeter voltage trend can expose severe drain but cannot separate sleep current, radio bursts, regulator loss, and LiPo discharge behavior. Use an in-series current instrument capable of resolving both microamp sleep and radio peaks for a defensible energy budget.
