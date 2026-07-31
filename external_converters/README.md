# Zigbee2MQTT External Converter

`zigbeehive-watermeter.mjs` is the canonical and only WaterMeter converter maintained by this repository. It is a standalone ES module for Zigbee2MQTT 2.x.

Install it as:

```text
<zigbee2mqtt-data>/external_converters/zigbeehive-watermeter.mjs
```

Zigbee2MQTT 2.11 and newer requires:

```yaml
advanced:
  enable_external_js: true
```

Audit the configured homelab before any deployment:

```bash
python3 tools/watermeter_harness.py z2m-audit
```

The audit is read-only. Deployment requires a clean, reviewed audit, its state-bound approval token, and the exact `DEPLOY Z2M` confirmation. See `.agents/skills/watermeter-z2m/SKILL.md` for legacy-bundle migration, backup, health checks, and rollback.

The converter exposes:

```text
pulse_count        read-only raw CurrentSummationDelivered pulses
scaled_summation   read-only pulse_count * scale_multiplier / scale_divisor
water_consumed     read-only liters for Home Assistant Energy
scale_multiplier   writable positive integer stored in device NVS
scale_divisor      writable positive integer stored in device NVS
multiplier         read-only standard Metering mirror
divisor            read-only standard Metering mirror
battery
voltage
```

The default scale is `10 / 1`, so one pulse is 10 L.

Unrelated Tuya, thermostat, or light-sensor converters belong to the Zigbee2MQTT installation that uses them. Do not add them to this firmware repository or combine them with the canonical WaterMeter converter.
