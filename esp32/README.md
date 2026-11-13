# ESP32 WiFi Bridge — BLDC-FOC

This folder contains the **Arduino sketch** that runs on an external ESP32 development board
(e.g. ESP32-WROOM-32 DevKitC). It bridges the STM32G431CB motor controller to a WiFi web
interface so a browser can command the motor and observe real-time telemetry.

## Architecture

```
Browser  ←── HTTP/JSON ──→  ESP32  ←── UART (half-duplex) ──→  STM32  ←── FOC ──→  Motor
(10 Hz poll)                (50 Hz)   (230400 baud, PB6/J7.1)  (20 kHz ISR)
```

## Hardware requirements

| Item | Detail |
|---|---|
| ESP32 board | Any ESP32 DevKit (WROOM-32, DevKitC, Wrover, etc.) |
| Power | USB (separate from the BLDC board; share GND) |
| UART | GPIO16 = RX, GPIO17 = TX, 230400 8N1 |
| Pull-up | 4.7 kΩ from the UART line to 3.3 V |
| Isolating resistor | 1 kΩ series on GPIO17 (TX) only |

## Wiring

```
BLDC board J7.1 (PB6) ──┬── 4.7 kΩ ── 3.3 V
                         ├── GPIO16 (ESP32 RX)
                         └── 1 kΩ ── GPIO17 (ESP32 TX)

BLDC board GND (J6.3 or J5.4) ── ESP32 GND
```

> **Important**: the ESP32 and the BLDC board must share a common GND even if powered separately.
> Never connect 5 V from the ESP32 USB to the BLDC board 3.3 V rail.

## AS5600 encoder (unchanged from standalone foc app)

The encoder I2C connection on the BLDC board is unaffected:

```
AS5600 SCL ── PA15 (J6.2)
AS5600 SDA ── PB7  (J7.2)   ← shares the J7 connector but different pin from the UART link
AS5600 VCC ── J6.1 (3.3 V)
AS5600 GND ── J6.3
```

The AS5600 `OUT` pin is **not** used (the firmware reads `RAW_ANGLE` over I2C for minimum latency).

## Building and flashing

1. Install [Arduino IDE 2.x](https://www.arduino.cc/en/software).
2. Add the ESP32 board package (Preferences → Additional Boards Manager URLs):
   ```
   https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
   ```
3. Install board: Boards Manager → search "esp32" → install **esp32 by Espressif Systems**.
4. Open `esp32/bldc_bridge/bldc_bridge.ino`.
5. Select board: **ESP32 Dev Module** (or your specific board).
6. Select the correct COM/serial port.
7. Edit `config.h` to set your desired WiFi password.
8. Upload.

No additional libraries are required; only the standard ESP32 Arduino core is used.

## Web interface

After flashing, connect to the WiFi network **BLDC-FOC** (password: `bldcfoc123`) and open:

```
http://192.168.4.1
```

The web UI shows:
- Link status (ONLINE / OFFLINE) and telemetry age
- Real-time RPM, q-axis / d-axis currents, encoder angle
- RPM trend chart (last ~30 s)
- iq reference slider (−4 A to +4 A)
- RPM limit slider (0–800)
- ENABLE / DISABLE / CLEAR FAULT buttons
- Emergency STOP button (zeroes current and disables immediately)

The browser polls `/api/state` at 10 Hz and sends a heartbeat every 500 ms.
If the browser tab is closed for more than 2 seconds, the ESP32 zeros iq and
disables the bridge. The STM32 has an independent watchdog: if no UART command
arrives for 500 ms it zeroes iq; after 1 s it disables the bridge.

## Protocol

See `../docs/esp32.md` for the full frame format, timing analysis, safety design,
and research notes on the ESP32 / STM32G431 interface.

## Troubleshooting

| Symptom | Likely cause |
|---|---|
| "OFFLINE" in web UI | UART not wired, wrong baud, missing pull-up |
| CRC errors high | Missing 1 kΩ series resistor on TX, ground loop, bad solder joint |
| Motor stops after 1 s | Web deadman triggered; keep the browser tab open |
| Motor won't enable | Encoder not calibrated, nFAULT asserted (clear via button) |
| "FAULT" badge | DRV8316 overcurrent or UVLO; click CLEAR FAULT |
