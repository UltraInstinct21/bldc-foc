# ESP32 WiFi Bridge — Interface Reference

This document describes the hardware wiring, communication protocol, safety
design, and web application for the ESP32 WiFi bridge used with the
`foc_esp` firmware application.

## 1. Overview

The bridge creates a browser-accessible control and telemetry interface for
the BLDC motor controller without a dedicated PC tool:

```
Browser  →  WiFi  →  ESP32  →  half-duplex UART  →  STM32  →  motor FOC
                                                               telemetry back
```

The ESP32 acts as a WiFi access point. A web page served from the ESP32
lets a user enable the drive, set the torque current (iq), and view live
telemetry at 10 Hz. Commands travel from the browser to the ESP32 over HTTP,
then over a half-duplex UART link to the STM32. Telemetry travels in the
opposite direction on the same wire.

**Why half-duplex on PB6?**  
PB7 (USART1_RX) doubles as I2C1_SDA for the AS5600 encoder. Using PB7 as
UART RX would disable the encoder. USART1 in HDSEL (half-duplex) mode uses
only the TX pin (PB6) as a bidirectional open-drain line, leaving PB7 free
for I2C.

**Safety watchdog chain:**  
Two independent watchdogs guard against a lost link. The STM32 side zeros iq
if no command arrives within 500 ms and disables the bridge after 1000 ms.
The ESP32 side sends a zero-torque disable frame if the browser heartbeat
stops for 2000 ms. See section 6 for details.

## 2. Research: Exact Models

### STM32G431CBT6 — USART1 half-duplex

| Item | Value |
|---|---|
| Peripheral | USART1 |
| TX pin | PB6, AF7 |
| Mode | HDSEL (half-duplex single-wire) |
| Pin type | Open-drain with external pull-up |
| Baud rate | 230400 |
| Clock source | PCLK2 at 170 MHz |
| Baud error | < 0.1 % |

After `EspLink_Init()` is called, USART1 switches from the 115200 debug
console to 230400 half-duplex. The two modes are mutually exclusive on the
same pin.

### ESP32-WROOM-32 — UART2

| Item | Value |
|---|---|
| Module | ESP32-WROOM-32 |
| Logic voltage | 3.3 V |
| UART peripheral | UART2 |
| RX pin | GPIO16 |
| TX pin | GPIO17 |
| Maximum baud rate | 5 Mbps |
| Level shifting needed | No — same 3.3 V logic as STM32 |

### Half-duplex wiring

The shared line requires three passive components:

| Component | Value | Location | Purpose |
|---|---|---|---|
| Series resistor | 1 kΩ | ESP32 GPIO17 (TX) | Prevents contention when both sides drive simultaneously |
| Pull-up resistor | 4.7 kΩ | Shared line to 3.3 V | Ensures the line rests high (open-drain idle state) |
| Common GND | — | J6 pin 3 | Essential for a valid logic reference |

### AS5600 analog OUT latency

The AS5600 provides two angle outputs: analog OUT and I2C register
RAW_ANGLE. The analog OUT is derived from the internal ANGLE register, which
is filtered with a 2.2 ms default lag. This lag makes it unusable for FOC
above approximately 50 rpm because the angle error grows faster than the
control loop can track.

The firmware therefore uses I2C RAW_ANGLE (no filter, approximately 40 µs
latency) via the I2C1 peripheral on PA15 (SCL) and PB7 (SDA). This is the
reason PB7 must remain on I2C SDA rather than being used as UART RX.

## 3. Pin Conflict Resolution

The board's J7 connector exposes two UART pins:

| J7 pin | STM32 pin | Primary use | Conflict |
|---|---|---|---|
| 1 | PB6 | USART1_TX (AF7) | None — dedicated to UART |
| 2 | PB7 | USART1_RX (AF7) | Also I2C1_SDA (AF4) for AS5600 |

If PB7 were configured as USART1_RX, the AS5600 encoder would be
unreachable and FOC would fail.

**Resolution:** USART1 is configured in HDSEL half-duplex mode, which uses
only PB6 as a bidirectional TX pin. PB7 is kept as I2C1_SDA throughout.
The ESP32 connects its GPIO17 TX through a 1 kΩ resistor to the same PB6
line; GPIO16 RX connects directly to PB6.

Before `EspLink_Init()` is called, USART1 operates as a 115200 debug
console (TX only) on PB6. After `EspLink_Init()`, the peripheral is
reconfigured to 230400 half-duplex. Debug output stops at that point.

## 4. Frame Protocol

All frames share a common header followed by a type-specific payload and a
16-bit CRC.

### Header (5 bytes)

| Offset | Field | Size | Value |
|---|---|---|---|
| 0 | Sync 0 | 1 | 0xA5 |
| 1 | Sync 1 | 1 | 0x5A |
| 2 | Type | 1 | 0x01 = CMD, 0x02 = TLM |
| 3 | Seq | 1 | Sequence counter, wraps at 255 |
| 4 | Len | 1 | Payload length in bytes |

### CRC

CRC-16/CCITT-FALSE: initialiser 0xFFFF, polynomial 0x1021, big-endian,
computed over [Type, Seq, Len, Payload]. Appended as two bytes
(high byte first) after the payload.

### CMD frame (ESP32 → STM32) — payload 10 bytes, total 17 bytes

| Offset | Field | Type | Description |
|---|---|---|---|
| 0 | flags | uint8 | See CMD flag bits below |
| 1 | iq_ma | int16 (BE) | Commanded q-axis current in milliamps |
| 3 | id_ma | int16 (BE) | Commanded d-axis current in milliamps (normally 0) |
| 5 | rpm_limit | uint16 (BE) | Speed guard in rpm |
| 7 | reserved | uint8[3] | Zero, reserved for future use |

### TLM frame (STM32 → ESP32) — payload 20 bytes, total 27 bytes

| Offset | Field | Type | Description |
|---|---|---|---|
| 0 | flags | uint8 | See TLM flag bits below |
| 1 | drv_fault | uint8 | DRV8316CT nFAULT state (0 = OK, 1 = fault) |
| 2 | id_ma | int16 (BE) | Measured d-axis current in milliamps |
| 4 | iq_ma | int16 (BE) | Measured q-axis current in milliamps |
| 6 | iq_ref_ma | int16 (BE) | Active iq reference in milliamps |
| 8 | rpm_x10 | int16 (BE) | Mechanical speed × 10 (e.g. 8000 = 800.0 rpm) |
| 10 | angle_raw | uint16 (BE) | AS5600 RAW_ANGLE (0–4095) |
| 12 | uptime_ms | uint32 (BE) | Milliseconds since boot |
| 16 | enc_errors | uint16 (BE) | Cumulative I2C error count |
| 18 | isr_loops | uint16 (BE) | FOC ISR count since last TLM (diagnostic) |

### CMD flag bits (byte 0 of CMD payload)

| Bit | Mask | Meaning |
|---|---|---|
| 0 | 0x01 | ENABLE — enable the bridge and FOC output |
| 1 | 0x02 | ESTOP — immediate iq=0, disable bridge |
| 2–7 | — | Reserved, must be zero |

### TLM flag bits (byte 0 of TLM payload)

| Bit | Mask | Meaning |
|---|---|---|
| 0 | 0x01 | BRIDGE_ENABLED — bridge is active |
| 1 | 0x02 | ENCODER_VALID — calibration record is valid |
| 2 | 0x04 | DRV_FAULT — DRV8316CT has an active fault |
| 3 | 0x08 | WATCHDOG_TRIP — link watchdog fired since last TLM |
| 4–7 | — | Reserved |

## 5. Timing

| Parameter | Value |
|---|---|
| Poll rate | 50 Hz (20 ms period) |
| CMD frame size | 17 bytes |
| CMD transmit time at 230400 | 0.74 ms |
| TLM frame size | 27 bytes |
| TLM transmit time at 230400 | 1.17 ms |
| Total round-trip | ~1.9 ms |
| Headroom per 20 ms cycle | ~18 ms |

The STM32 parser discards frames where the Type field does not match the
expected direction, so echo of the CMD frame transmitted on the shared line
is ignored without raising a CRC error.

Debug text is printed at a 2 s rate before `EspLink_Init()`. The collision
probability once the link is active is negligible; at 115200 a 60-byte debug
line takes approximately 0.5 ms. Against a 20 ms cycle the duty cycle is
0.025 %, and after `EspLink_Init()` debug output ceases entirely.

## 6. Safety Design

Two independent watchdogs operate in parallel. Either one can disable the
drive independently of the other.

### STM32-side watchdog

| Condition | Action |
|---|---|
| No CMD received for 500 ms | iq reference forced to zero; drive stays enabled |
| No CMD received for 1000 ms | Bridge disabled; DRVOFF asserted |

The bridge also stays disabled at boot until a CMD frame with the ENABLE
flag is received. Encoder calibration must be valid before ENABLE is
accepted.

### ESP32-side watchdog

| Condition | Action |
|---|---|
| No browser heartbeat for 2000 ms | iq set to zero, ENABLE flag cleared, disable CMD sent |

The browser sends a heartbeat by polling `/api/set` every 500 ms. If the
browser tab is closed or the connection drops, the ESP32 watchdog fires
within 2 s and the STM32 watchdog fires within a further 1 s.

## 7. Web Application

The ESP32 serves a single-page HTML application from PROGMEM at
`http://192.168.4.1`. No external libraries, CDN assets, or build tools are
required.

| Endpoint | Method | Purpose |
|---|---|---|
| `/` | GET | Serve the HTML page |
| `/api/state` | GET | Return JSON telemetry snapshot (10 Hz polling) |
| `/api/set` | GET/POST | Send iq command and heartbeat (every 500 ms) |
| `/api/estop` | POST | Immediate emergency stop — sets ESTOP flag |

The ESP32 WiFi stack runs in AP mode (SSID: `bldc-foc`, no password by
default). The WebServer is single-threaded; the web UI should not issue
concurrent requests.

## 8. Wiring Diagram

```
BLDC Board              ESP32
──────────────────      ───────────────
J7.1 PB6  ─────────────── GPIO16 (RX)
           │── 1 kΩ ───── GPIO17 (TX)
           └── 4.7 kΩ ─── 3.3 V

J6.3 GND ─────────────── GND  (essential)

J7.2 PB7 ─── AS5600 SDA  (I2C, unchanged)
J6.2 PA15 ── AS5600 SCL  (I2C, unchanged)
J6.1 3.3V ── AS5600 VCC
J6.3 GND  ── AS5600 GND
```

The 1 kΩ series resistor on GPIO17 must be fitted. Without it, simultaneous
drive from both sides will cause contention and may damage GPIO17 on the
ESP32.

## 9. Known Limitations

- **Half-duplex contention.** Only one side may drive the line at a time. If
  both sides transmit simultaneously, the receiving parser will see a CRC
  failure and discard the frame. Recovery happens on the next 20 ms cycle.
  This is rare in practice because the STM32 transmits TLM only in response
  to a received CMD.

- **Debug output on the shared line.** `foc_esp` keeps debug prints to a
  minimum to reduce the chance of collision before `EspLink_Init()`. After
  `EspLink_Init()` the UART is reconfigured and no further debug text is
  emitted.

- **ESP32 single-core scheduling.** WiFi and UART polling run on the same
  ESP32 core. At high web traffic (e.g., multiple browser tabs or rapid page
  refreshes), UART polling may jitter by ±5 ms. This is within the 500 ms
  watchdog margin and is acceptable.

- **Single-threaded web server.** The ESP32 `WebServer` library handles one
  request at a time. The web UI should not issue overlapping requests.

- **No TLS.** The web interface is plain HTTP within the local WiFi AP. It
  is not suitable for use on untrusted networks.

## 10. Further Reading

- [`../firmware/foc_esp/README.md`](../firmware/foc_esp/README.md) — `foc_esp` application and tuning constants
- [`../esp32/README.md`](../esp32/README.md) — ESP32 firmware build and flash instructions
- [`bringup.md`](bringup.md) — ordered board bring-up and test procedure
- [`hardware.md`](hardware.md) — full pin map, connectors, and board errata
