---
name: watermeter-z2m
description: Audit, migrate, deploy, verify, and roll back the ZigbeeHive watermeter external converter on the homelab Zigbee2MQTT installation. Use for converter drift, Zigbee2MQTT support, remote configuration, external converter migration, service restart, health checks, or rollback involving homelab.lan.
---

# Watermeter Zigbee2MQTT

Use the canonical local converter at `external_converters/zigbeehive-watermeter.mjs`. The default remote installation is:

```text
SSH host:     homelab.lan
Data:         /mnt/projects/smart_home/zigbee2mqtt-data
Compose file: /mnt/projects/smart_home/compose.yml
Service:      zigbee2mqtt
Container:    zigbee2mqtt
```

The canonical local device icon is `external_converters/device_icons/zigbeehive-watermeter.png`. Install it as `device_icons/zigbeehive-watermeter.png` under the Zigbee2MQTT data directory and reference that relative path from the device entry in `configuration.yaml`.

Use SSH configuration or an SSH agent. Never place credentials or private keys in the repository.

## Audit First

Always begin with the read-only operation:

```bash
python3 tools/watermeter_harness.py z2m-audit
```

The audit checks the compose service, `advanced.enable_external_js`, converter syntax inside the running Zigbee2MQTT container, the canonical file diff, and duplicate legacy WaterMeter definitions. It may display a deployment diff but must not write or restart anything.

## Handle The One-Time Legacy Split

The existing homelab may contain one CommonJS aggregate with unrelated Tuya/TS0222 converters and a copied WaterMeter definition. This repository must not own those unrelated definitions.

If audit reports a legacy conflict:

1. Read and preserve the complete remote aggregate.
2. Prepare a timestamped backup under `/mnt/projects/smart_home/zigbee2mqtt-data/.watermeter-backups`.
3. Split only the unrelated definitions into a remote-only converter file. Preserve their imports, fingerprints, exposes, datapoints, and exports exactly.
4. Remove the WaterMeter definition from the legacy file and stage the canonical `.mjs` as a separate file.
5. Move the old aggregate outside the auto-loaded `.js`/`.mjs` paths or give the backup a non-JavaScript suffix.
6. Validate syntax in the Zigbee2MQTT container and show the exact remote diff.
7. Ask for explicit approval before writing files or restarting the service.

Do not automate this split with substring deletion when the remote file shape is uncertain. Never alter unrelated converter behavior to make the migration cleaner.

## Deploy The Canonical Converter

Deploy only after audit is clean and the user explicitly approves the shown diff and restart. Pass the token printed by that separately reviewed audit so deployment is bound to the same local and remote converter state:

```bash
python3 tools/watermeter_harness.py z2m-deploy \
    --approved-audit <token-from-z2m-audit> \
    --confirm 'DEPLOY Z2M'
```

The command reruns the audit and refuses a stale or missing token. It then backs up the current WaterMeter target, uploads to a temporary `.mjs`, checks syntax inside the container, atomically replaces the target, restarts only `zigbee2mqtt`, and prints status and recent logs. It refuses deployment while another file still defines WaterMeter.

Do not change `configuration.yaml`, other services, devices, or unrelated converters as a side effect. If `advanced.enable_external_js` is disabled, prepare and show that configuration change separately and request approval.

## Verify

After restart, require all of the following:

- the `zigbee2mqtt` service is running;
- no external-converter syntax/load error appears in recent logs;
- model `ZigbeeHive-WaterMeter` is supported;
- existing unrelated devices remain supported;
- `pulse_count`, `scaled_summation`, `water_consumed`, battery, and voltage remain available;
- writable scale values remain positive and preserve their previous values.
- the configured device icon path exists and remains scoped to the WaterMeter device.

Do not generate test pulses remotely. Use the hardware-test skill when an end-to-end pulse is required.

## Roll Back

Never roll back silently. Show the retained backup timestamp and request a separate confirmation:

```bash
python3 tools/watermeter_harness.py z2m-rollback \
    --backup 20260731T153000Z \
    --confirm 'ROLLBACK Z2M'
```

The generic rollback command restores only a standalone canonical-converter deployment. It must refuse a one-time legacy-split backup marked by `MIGRATION.txt`; restoring that backup requires the complete configuration and converter set described in the marker and a separately reviewed command.

Restart only Zigbee2MQTT and repeat the health checks. Never delete old backups automatically.
