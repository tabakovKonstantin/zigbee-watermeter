# Repository Guidance

These rules apply to the entire repository.

## Read First

- Use `README.md` for the hardware, wiring, Zigbee contract, sleep policy, and architecture.
- Use `.agents/skills/watermeter-firmware/SKILL.md` for build, flash, logs, diagnostics, erase, and OTA.
- Use `.agents/skills/watermeter-hardware-test/SKILL.md` for physical Hall, sleep, Zigbee delivery, and battery tests.
- Use `.agents/skills/watermeter-z2m/SKILL.md` for the remote Zigbee2MQTT converter.
- Use `tools/watermeter_harness.py`; do not hardcode local ESP-IDF paths, serial ports, or partition offsets.

## Behavior Invariants

- Target ESP32-C6 with ESP-IDF v5.4.1, gnu17, `MINIMAL_BUILD`, and the pinned component versions.
- Preserve endpoint 1, standard Metering/Battery attributes, custom attributes `0xFC00`/`0xFC01`/`0xFC02`, and Zigbee2MQTT report formats.
- Preserve OTA magic `0x0BEEF11E`, manufacturer `0x131B`, image type `0x0001`, dual-slot layout, and rollback behavior.
- Keep `sdkconfig.defaults` and `partitions.csv` as sources of truth.
- Keep every accepted pulse committed to NVS immediately. Do not batch, defer, or remove the per-pulse commit without explicit approval.
- Do not count GPIO1 already low on ordinary power-on/reset. Do not create phantom counts on release.
- Do not change power policy, timing, reporting, commissioning, or OTA behavior without explicit approval and hardware verification.

## Safety

- Ordinary flash must preserve NVS and Zigbee storage.
- Parse erase offsets and sizes from `partitions.csv`.
- Require exact typed confirmation before erasing Zigbee storage, erasing all flash, remote deploy, remote restart, or rollback. Bind deploy approval to the state token from a separately reviewed audit.
- Treat test magnet pulses as permanent 10 L increments. Warn and get approval; never compensate the NVS counter afterward.
- Audit and diff `homelab.lan` before writes. Back up first, restart only Zigbee2MQTT, verify health, and never delete backups automatically.
- Never commit secrets, SSH keys, generated `sdkconfig`, build output, editor state, or local serial-port assumptions.

## Workflow

1. Inspect `git status` and preserve unrelated user changes.
2. Make narrowly scoped changes using existing module boundaries and patterns.
3. Run `python3 tools/watermeter_harness.py test`.
4. Run production and diagnostic builds when firmware, configuration, build tooling, or operational tooling changes.
5. Confirm every `watermeter.bin` is strictly smaller than the OTA slot parsed from `partitions.csv`.
6. Report what was verified and what still requires physical hardware or Zigbee2MQTT observation.

## Style

- C: Espressif style, 4 spaces, K&R braces, snake_case, `s_` file-scope statics, uppercase macros, approximately 120 columns.
- Python: standard library first, explicit errors, no dependencies for the harness.
- Keep comments, logs, documentation, and user-visible project strings in English.
- Prefer KISS/YAGNI and SRP. Add abstractions only when they remove real duplication or isolate a real responsibility.
- Do not modify unrelated files or revert user changes.
