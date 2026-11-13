# test_07_esp_link — ESP32 UART Link Test

`test_07_esp_link` verifies the half-duplex USART1 link between the STM32G431
and an external ESP32 **without connecting or spinning the motor**.

## What it tests

- USART1 half-duplex (HDSEL) on PB6 (J7.1) at 230400 baud.
- Frame transmission and reception using CRC-16/CCITT-FALSE.
- Local echo suppression (discarding own TLM reflections on the half-duplex line).
- Bidirectional communication: responding to ESP32 poll frames with telemetry.
- Web UI end-to-end verification (connecting to WiFi `BLDC-FOC` and viewing values).

## Hardware setup

Wire the ESP32 to J7.1 (PB6) as described in `../../docs/esp32.md`:

```
BLDC board J7.1 (PB6) ──┬── 4.7 kΩ ── 3.3 V
                         ├── GPIO16 (ESP32 RX)
                         └── 1 kΩ ── GPIO17 (ESP32 TX)

BLDC board GND (J6.3 or J5.4) ── ESP32 GND
```

No motor connections or encoder connections are required for this test.

## How to run

1. Flash the ESP32 with `esp32/bldc_bridge/bldc_bridge.ino`.
2. Connect to WiFi access point `BLDC-FOC` on your phone or laptop.
3. Open `http://192.168.4.1` in a browser.
4. Flash the STM32:
   ```bash
   cd firmware
   make APP=test_07_esp_link flash
   ```
5. Check the web dashboard:
   - Status badge transitions from **OFFLINE** to **ONLINE**.
   - Angle increments smoothly from 0 to 4095.
   - Speed trends across the chart.
   - Dragging the `iq ref` slider updates the `q-axis current (reference)` readback.
