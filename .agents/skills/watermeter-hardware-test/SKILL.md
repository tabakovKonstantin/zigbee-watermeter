---
name: watermeter-hardware-test
description: Guide a human through safe functional and battery testing of the XIAO ESP32-C6 Zigbee water meter. Use when validating Hall pulses, stuck-low handling, phantom-pulse prevention, watchdog behavior, deep-sleep wake/release behavior, Zigbee2MQTT delivery, production restoration, or battery drain on the physical device.
---

# Watermeter Hardware Test

Run hardware validation as a human-in-the-loop procedure. Ask for one physical action at a time and wait for the result. Never pretend that a GPIO, magnet, reset button, USB cable, Zigbee coordinator, or meter was manipulated automatically.

## Protect Meter Data

- Read the current `pulse_count` from logs or Zigbee2MQTT before generating a test pulse.
- Warn that every accepted magnet pulse permanently adds one count, equal to 10 L with the production scale.
- Require explicit approval before test pulses.
- Never erase, decrement, rewrite, or compensate the NVS counter after a test.
- Record the expected and observed counter delta.

## Phase 1: Functional Diagnostic Test

Use USB power and the diagnostic profile. This phase checks behavior, not energy consumption.

1. Run `python3 tools/watermeter_harness.py doctor` and `ports`.
2. Flash with `python3 tools/watermeter_harness.py flash --profile diagnostic`. Do not erase flash.
3. Ask the user to press RESET once without BOOT if the board remains in ROM download mode.
4. Start `monitor --profile diagnostic` with a TTY and capture the baseline pulse count, boot cause, network state, and absence of NVS errors.
5. Confirm in Zigbee2MQTT that the device is recognized as `ZigbeeHive-WaterMeter` before pulse tests.

### Single Pulse

1. Confirm the Hall output is released/high.
2. Ask the user to apply the magnet once and then remove it.
3. Verify exactly one `Pulse counted` entry and a counter delta of one.
4. Reject repeated counts, queue floods, watchdog messages, or NVS errors.
5. Allow delivery on the current or a later scheduled wake; do not require an unnecessary retry loop.

### Stuck-Low And Release

1. Confirm the user accepts one additional permanent test pulse.
2. Ask the user to apply and hold the magnet for at least 15 seconds.
3. Verify one pulse only, no task-watchdog trigger, and no repeated count while the output remains low.
4. Expect deep sleep to arm a high-level wake while waiting for release; the USB port may disappear.
5. Ask the user to remove the magnet.
6. Verify the release wake rearms the low-level wake without incrementing the counter or starting a full Zigbee session.
7. Apply one later normal pulse to prove the input was rearmed; expect one additional count only.

### Reset And Phantom Pulse

1. With the Hall output released/high, reset and verify no count.
2. Hold the magnet before an ordinary RESET and keep it held through boot.
3. Verify an ordinary power-on/reset with GPIO1 already low does not count a pulse.
4. Remove the magnet and verify release does not count.
5. Stop immediately on any phantom increment or watchdog output and retain the complete log.

### Zigbee2MQTT Delivery

Verify that `pulse_count`, `scaled_summation`, and `water_consumed` converge to the expected values in Zigbee2MQTT. A mandatory delivery attempt may stop after 10 seconds and retry on a later wake. Treat eventual correct delivery without data loss as success; do not keep the device awake merely to force an immediate UI update.

## Restore Production

After functional testing:

```bash
python3 tools/watermeter_harness.py flash --profile production
```

Press RESET once without BOOT if needed. Confirm production boot, then disconnect USB. Do not leave diagnostic firmware installed for power testing.

## Phase 2: Battery Test

- Use the production profile and the final external U.FL antenna and enclosure placement.
- Power only from the 3.7 V LiPo through `BAT+`/`BAT-`; disconnect USB.
- Keep the Zigbee coordinator and parent in their normal production locations.
- Record battery voltage, timestamps, real water activity, coordinator outages, and resets over several days.
- A multimeter voltage trend can expose severe drain but cannot separate deep-sleep current, regulator loss, radio retries, or LiPo discharge-curve effects.
- For a defensible energy budget, measure current in series with the battery using equipment that resolves sleep current and radio bursts.

Do not promise a battery lifetime from voltage snapshots alone. Report observed conditions and uncertainty.
