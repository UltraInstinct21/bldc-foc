# foc_esp — FOC with ESP32 WiFi Link

`foc_esp` is a dedicated variant of the `foc` application that adds a half-duplex
UART link to an external ESP32. The ESP32 serves a browser-based control and
telemetry dashboard over WiFi.

The encoder source is selectable at build time:

```bash
make APP=foc_esp ENCODER_MODE=i2c   # direct AS5600 I2C, original path
make APP=foc_esp ENCODER_MODE=adc   # ESP32 ADC -> UART -> STM32
```

For `ENCODER_MODE=adc`, set `AS5600_ADC_ENABLED` to `1` in
`esp32/bldc_bridge/config.h` and upload the ESP32 sketch. For I2C mode, leave
it at `0`.

## Differences from foc

| Feature | foc | foc_esp |
|---|---|---|
| Encoder | AS5600 I2C (PA15/PB7) | ESP32 ADC GPIO34 -> UART |
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

Connect AS5600 OUT to ESP32 GPIO34, and keep AS5600 VCC/GND connected to the
3.3 V and ground domain shared with the ESP32. The STM32 does not initialize
I2C in this application. The ESP32 samples OUT four times, converts 0..3.3 V
to a 0..4095 angle, and places that angle in every 5 ms command frame.

## Compile and flash

```bash
cd firmware
make APP=foc_esp flash
```

## Runtime behaviour

1. Boot → ESP32 ADC link → encoder calibration (or flash load) → current offset calibration.
2. `EspLink_Init(230400)` switches USART1 on PB6 to half-duplex open-drain mode.
3. Bridge remains **disabled** until the web UI sends an `ENABLE` command.
4. The ESP32 samples the encoder and polls at 200 Hz; each poll triggers one immediate telemetry reply.
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
