# ESP32-H2 Zigbee Water Meter

This project implements a Zigbee Water Meter using the **ESP32-H2** and the **ESP-IDF** framework. It counts pulses from a standard water meter sensor (i.e. KH TL-W5MC1 5mm 5v), tracks total usage in liters, and reports the data to a Zigbee Coordinator (like Home Assistant w/ ZHA or Zigbee2MQTT) via the standard **Metering Cluster**.

## Features

*   **Pulse Counting**: Reads pulses on a GPIO pin (default: GPIO 13) to calculate water usage.
*   **Persistent Storage**: Saves the total water count to NVS (Non-Volatile Storage), preserving data across reboots and power outages.
*   **Zigbee Metering Cluster**:
    *   Reports "Current Summation Delivered" (Total usage).
    *   Configured for Cubic Meters ($m^3$) with a multiplier/divisor (1 pulse = 1 Liter = 0.001 $m^3$).
*   **Automatic Reporting**: Sends updates to the coordinator when usage changes.
*   **Debouncing**: Software debouncing (50ms) to prevent false counts from mechanical switches.

## Hardware Requirements

1.  **ESP32-H2 Development Board** (e.g., ESP32-H2-DevKitM-1).
2.  **Proximity sensor (TL-W5MC1)**: A sensor that outputs a pulse for every X amount of water (usually 1 pulse = 1 Liter).
3.  **Connecting Wires**.

### Wiring

| Water Meter Sensor | ESP32-H2 |
| :--- | :--- |
| Signal / Pulse | **GPIO 13** |
| GND | **GND** |
| VCC (if required) | 3.3V or 5V (depending on sensor) |

> **Note**: The code enables the internal pull-up resistor on GPIO 13, so a simple reed switch connected between GPIO 13 and GND will work.

## Software Requirements

*   **ESP-IDF v5.0+** (Recommended: v5.1 or v5.2 for ESP32-H2 support).
*   **Espressif Zigbee SDK** (installed automatically via `idf_component.yml`).

## Build and Flash

1. Install esp-idf at `$HOME/esp/esp-idf`

1.  **Set up the environment**:
    ```bash
    . $HOME/esp/esp-idf/export.sh
    ```

2.  **Set the target**:
    ```bash
    idf.py set-target esp32h2
    ```

3.  **Build the project**:
    ```bash
    idf.py build
    ```

4.  **Flash and Monitor**:
    ```bash
    idf.py flash monitor
    ```

## How It Works

### 1. Pulse Counting
*   A GPIO interrupt (`gpio_isr_handler`) triggers on the falling edge of the signal (connection to ground).
*   The handler uses a **50ms debounce** timer to ignore noise.
*   Valid pulses increment a global `total_liters_count`.

### 2. Data Persistence
*   A background task (`update_task`) runs every 1 second.
*   If the count has changed, it:
    1.  Updates the Zigbee stack's internal attribute value.
    2.  Saves the new count to the **NVS partition** (`water_count` key).
*   On boot, the system restores the last known count from NVS.

### 3. Zigbee Communication
*   **Role**: End Device (ED).
*   **Cluster**: Metering Cluster (0x0702).
*   **Attributes**:
    *   `CurrentSummationDelivered` (0x0000): The main reading.
    *   `UnitOfMeasure` (0x0300): Cubic Meters.
    *   `Multiplier` (0x0301): 1.
    *   `Divisor` (0x0302): 1000.
    *   (Result = Raw * 1 / 1000 = $m^3$).
*   **Reporting**: The device is configured to report changes of at least 1 unit (1 liter) automatically.

## Usage

1.  **Pairing**:
    *   On first boot, the device will automatically enter "Network Steering" mode to join an open Zigbee network.
    *   Enable "Join" mode on your Zigbee Coordinator.
    *   Monitor the logs; you should see `Joined network successfully!`.

2.  **Home Assistant / ZHA**:
    *   Once joined, it should appear as a "Water Meter" device.
    *   It will report entities for the total summation.

3.  **Resetting**:
    *   The code handles factory reset signals if triggered via standard Zigbee commands (e.g., "Leave Network").
    *   You can manually erase NVS (`idf.py erase-flash`) to reset the counter and network settings.
