# Codex Notes

For this project, run ESP-IDF commands by activating the ESP-IDF v5.4.1 environment in the same shell command.

Do not rely on bare `idf.py` being present in `PATH` in non-interactive shells. Use the full `idf.py` path after activation:

```bash
. '/Users/konstantin.tabakov/.espressif/tools/activate_idf_v5.4.1.sh' >/dev/null && /Users/konstantin.tabakov/.espressif/tools/python/v5.4.1/venv/bin/python3 /Users/konstantin.tabakov/.espressif/v5.4.1/esp-idf/tools/idf.py
```

Examples:

```bash
. '/Users/konstantin.tabakov/.espressif/tools/activate_idf_v5.4.1.sh' >/dev/null && /Users/konstantin.tabakov/.espressif/tools/python/v5.4.1/venv/bin/python3 /Users/konstantin.tabakov/.espressif/v5.4.1/esp-idf/tools/idf.py fullclean
. '/Users/konstantin.tabakov/.espressif/tools/activate_idf_v5.4.1.sh' >/dev/null && /Users/konstantin.tabakov/.espressif/tools/python/v5.4.1/venv/bin/python3 /Users/konstantin.tabakov/.espressif/v5.4.1/esp-idf/tools/idf.py build
. '/Users/konstantin.tabakov/.espressif/tools/activate_idf_v5.4.1.sh' >/dev/null && /Users/konstantin.tabakov/.espressif/tools/python/v5.4.1/venv/bin/python3 /Users/konstantin.tabakov/.espressif/v5.4.1/esp-idf/tools/idf.py -p /dev/cu.usbmodem11101 flash
. '/Users/konstantin.tabakov/.espressif/tools/activate_idf_v5.4.1.sh' >/dev/null && /Users/konstantin.tabakov/.espressif/tools/python/v5.4.1/venv/bin/python3 /Users/konstantin.tabakov/.espressif/v5.4.1/esp-idf/tools/idf.py -p /dev/cu.usbmodem11101 monitor
```

When running monitor from Codex tools, `idf.py monitor` may fail because it requires stdin attached to a TTY. Use direct `idf_monitor.py` with `tty: true`:

```bash
. '/Users/konstantin.tabakov/.espressif/tools/activate_idf_v5.4.1.sh' >/dev/null && /Users/konstantin.tabakov/.espressif/tools/python/v5.4.1/venv/bin/python3 /Users/konstantin.tabakov/.espressif/v5.4.1/esp-idf/tools/idf_monitor.py -p /dev/cu.usbmodem11101 -b 115200 --toolchain-prefix riscv32-esp-elf- --target esp32c6 --revision 0 --decode-panic backtrace /Users/konstantin.tabakov/Home/esp/zigbee-watermeter/build/watermeter.elf
```

Use `/dev/cu.usbmodem11101` for ESP32-C6 USB serial on this machine.

If Zigbee reports `Failed to find zb_storage partition`, check `partitions.csv` and `sdkconfig`, then run `fullclean` before rebuilding. The project must use `CONFIG_PARTITION_TABLE_CUSTOM=y` and `CONFIG_PARTITION_TABLE_FILENAME="partitions.csv"`.
