# foc_esp — FOC with ESP32 WiFi Link

`foc_esp` is a dedicated variant of the `foc` application that adds a half-duplex
UART link to an external ESP32. The ESP32 serves a browser-based control and
telemetry dashboard over WiFi.

## Differences from foc

| Feature | foc | foc_esp |
|---|---|---|
| Encoder | AS5600 I2C (PA15/PB7) | AS5600 I2C (PA15/PB7) — identical |
| Debug / link | USART1 TX (115200 baud) | USART1 half-duplex (230400 baud) |
| Motor start | Automatic ramp at boot | Requires ENABLE command from web UI |
| iq reference | Fixed `APP_IQ_A` | Commanded by web UI (−4.0 to +4.0 A) |
| Link watchdog | None | 500 ms zero-iq, 1000 ms bridge disable |

## Hardware wiring

Wire the ESP32 to J7.1 (PB6) as described in `../../docs/esp32.md`:

```
BLDC board J7.1 (PB6) ──┬── 4.7 kΩ ── 3.3 V
                         ├── GPIO16 (ESP32 RX)
                         └── 1 kΩ ── GPIO17 (ESP32 TX)

BLDC board GND (J6.3 or J5.4) ── ESP32 GND
```

The AS5600 encoder remains wired to J6.2 (PA15 / SCL) and J7.2 (PB7 / SDA).
The ESP32 uses only J7.1 (PB6) in half-duplex mode, avoiding any pin conflict with the encoder.

## Compile and flash

```bash
cd firmware
make APP=foc_esp flash
```

## Runtime behaviour

1. Boot → AS5600 init → encoder calibration (or flash load) → current offset calibration.
2. `EspLink_Init(230400)` switches USART1 on PB6 to half-duplex open-drain mode.
3. Bridge remains **disabled** until the web UI sends an `ENABLE` command.
4. The ESP32 polls at 50 Hz; each poll triggers one immediate telemetry reply.
5. If communication stops for 500 ms, target current is zeroed; after 1000 ms the bridge is disabled.

## Tuning constants (foc_esp/main.c)

| Constant | Default | Description |
|---|---|---|
| `APP_IQ_MAX_A` | 4.0 | Hard clamp on commanded iq (A) |
| `APP_IQ_RAMP` | 2.0 | A/s rate limit for current changes |
| `APP_RPM_LIMIT` | 800.0 | Mechanical RPM upper guard |
| `LINK_WATCHDOG_MS` | 500 | Zero-iq timeout on link loss (ms) |
| `LINK_DISABLE_MS` | 1000 | Bridge disable timeout on link loss (ms) |

## Files

| File | Purpose |
|---|---|
| `main.c` | Application entry, state machine, safety watchdogs, telemetry packaging |
| `esp_link.h` | Frame format, command / telemetry structs, link API |
| `esp_link.c` | USART1 half-duplex driver, CRC-16/CCITT-FALSE, parser state machine |
