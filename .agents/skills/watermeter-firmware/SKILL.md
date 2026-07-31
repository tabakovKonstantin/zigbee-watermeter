---
name: watermeter-firmware
description: Build, test, flash, monitor, diagnose, and prepare USB or Zigbee OTA updates for the zigbee-watermeter ESP-IDF firmware. Use for firmware operations, serial-port discovery, production or diagnostic profiles, partition erasure, OTA image creation, binary-size checks, or ESP32-C6 troubleshooting in this repository.
---

# Watermeter Firmware

Use `tools/watermeter_harness.py` from the repository root. Treat `sdkconfig.defaults`,
`sdkconfig.diagnostic.defaults`, and `partitions.csv` as the configuration sources of truth.

## Preserve Invariants

- Use ESP-IDF v5.4.1 and target `esp32c6`.
- Keep every pulse committed to NVS immediately.
- Preserve Zigbee endpoint 1, Metering and Battery attributes, custom attributes `0xFC00`-`0xFC02`, and report formats.
- Preserve OTA magic `0x0BEEF11E`, manufacturer `0x131B`, image type `0x0001`, and dual-slot compatibility.
- Require `watermeter.bin` to be strictly smaller than the OTA slot parsed from `partitions.csv`.
- Never hardcode a serial port or partition offset. Discover the port and parse the table.
- Never erase NVS or Zigbee storage as part of an ordinary build, flash, diagnostic, or update.

## Choose The Operation

Run the environment check first on a new machine or after an ESP-IDF change:

```bash
python3 tools/watermeter_harness.py doctor
python3 tools/watermeter_harness.py ports
```

Run local tests and a production build after firmware or tooling changes:

```bash
python3 tools/watermeter_harness.py test
python3 tools/watermeter_harness.py build
python3 tools/watermeter_harness.py check-size
```

Use `--clean` only when configuration or generated build state is suspect. It removes only the selected build directory.

## Flash Safely

Build and flash production firmware without erasing state:

```bash
python3 tools/watermeter_harness.py flash --profile production
```

Pass `--port` only when more than one candidate is connected. After native USB flashing, the XIAO may remain in ROM download mode. Press RESET once without holding BOOT. Do not erase the board to fix this condition.

Monitor with the ELF matching the flashed profile:

```bash
python3 tools/watermeter_harness.py monitor --profile production
```

When an agent starts the monitor, allocate a TTY. Expect the USB port to disappear during deep sleep.

## Use Diagnostic Mode

Keep diagnostic output isolated from production artifacts:

```bash
python3 tools/watermeter_harness.py build-diagnostic
python3 tools/watermeter_harness.py flash --profile diagnostic
python3 tools/watermeter_harness.py monitor --profile diagnostic
```

Diagnostic mode enables INFO logging and USB Serial/JTAG. It preserves NVS and Zigbee pairing but changes awake time and board current. Never use it for battery-life measurements. Restore production firmware after diagnosis with `flash --profile production`.

## Erase Only With Explicit Approval

Use `erase-zigbee` only to recover corrupted pairing state. It reads the current `zb_storage` offset and size from `partitions.csv`, preserves pulse NVS, and requires re-pairing:

```bash
python3 tools/watermeter_harness.py erase-zigbee --confirm 'ERASE ZIGBEE'
```

Use `erase-all` only after the user explicitly accepts losing firmware, pulse count, OTA state, and Zigbee pairing:

```bash
python3 tools/watermeter_harness.py erase-all --confirm 'ERASE ALL'
```

Never infer either confirmation from a general request to flash, clean, repair, or retry.

## Prepare OTA

Require a user-selected monotonically increasing file version:

```bash
python3 tools/watermeter_harness.py ota-build --file-version 42
```

The command builds production firmware, checks its slot size, and writes the `.ota` image and index under `build/ota`. It does not push Git or publish a release. CI uses `github.run_number` and publishes from `main` through GitHub Pages.

## Finish A Change

Run Python/C tests, the production build, and the diagnostic build. Report both binary sizes and any test that could not run. Do not claim a hardware, Zigbee delivery, deep-sleep current, or OTA result from compilation alone.
