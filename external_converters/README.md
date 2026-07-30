# Zigbee2MQTT external converter

Converter file:

```text
external_converters/zigbeehive-watermeter.mjs
```

The `.mjs` file is the canonical WaterMeter converter. After changing it, regenerate the WaterMeter
definition embedded in the legacy CommonJS bundle:

```bash
python3 tools/sync_watermeter_converter.py
python3 tools/sync_watermeter_converter.py --check
```

Zigbee2MQTT expects external converters in the `external_converters` directory next to its `configuration.yaml`.

Example target path:

```text
<zigbee2mqtt-data>/external_converters/zigbeehive-watermeter.mjs
```

For Zigbee2MQTT 2.11.0 and newer, external JavaScript converters are disabled by default. Enable them in Zigbee2MQTT first.

After copying the file, restart Zigbee2MQTT or add/update it from:

```text
Zigbee2MQTT UI -> Settings -> Dev console -> External converters
```

The converter exposes:

```text
pulse_count        read-only, raw CurrentSummationDelivered pulses
scaled_summation   read-only, pulse_count * scale_multiplier / scale_divisor
scale_multiplier   writable, stored in device NVS
scale_divisor      writable, stored in device NVS
multiplier         read-only standard metering mirror
divisor            read-only standard metering mirror
battery
voltage
```

Default water-meter scale:

```text
scale_multiplier = 10
scale_divisor = 1
```

That means one pulse equals 10 scaled units.
