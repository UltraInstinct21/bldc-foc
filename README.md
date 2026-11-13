# bldc-foc

Sensored field-oriented control firmware for a custom 24 V BLDC drive:

- STM32G431CBT6 (170 MHz, LQFP-48)
- DRV8316CT gate driver, 6x PWM in hardware mode, integrated current sense (0.3 V/A)
- AS5600 magnetic encoder on I2C1
- 24 V outrunner, 7 pole pairs, 5 Arms, 2000 rpm
- External ESP32 WiFi bridge for browser-based telemetry and motor control

The firmware is split into independent test applications, one per board
function, plus standalone FOC and FOC + ESP32 WiFi bridge applications. Each app
links the same board layer and is flashed on its own.

## Layout

| Path | Contents |
|---|---|
| `firmware/` | Build system and STM32 source code |
| `firmware/common/` | Clock, GPIO, pin AFs, interrupts, UART console & link |
| `firmware/foc/` | Standalone FOC application (I2C encoder, automatic start) |
| `firmware/foc_esp/` | FOC application with ESP32 half-duplex UART link |
| `firmware/tests/` | Eight independent hardware test apps (`test_00` to `test_07`) |
| `firmware/Drivers/` | ST HAL and CMSIS |
| `esp32/` | Arduino sketch and web app for external ESP32 WiFi bridge |
| `hardware/` | KiCad schematic and PCB for board revision v6 |
| `docs/` | Hardware, motor, control, bring-up, and ESP32 interface documentation |

## Quick start

### STM32 firmware
Requirements: `arm-none-eabi-gcc`, OpenOCD, and an ST-Link probe.

```sh
cd firmware

make APP=test_00_hello flash    # first board check
make APP=test_07_esp_link flash # verify ESP32 link without motor
make APP=foc flash              # standalone FOC app
make APP=foc_esp flash          # FOC + ESP32 WiFi control app
make apps                       # build every app
```

Full build and flash details are in [`firmware/README.md`](firmware/README.md).

### ESP32 WiFi bridge
Flash `esp32/bldc_bridge/bldc_bridge.ino` using the Arduino IDE.
Connect to WiFi **BLDC-FOC** (password `bldcfoc123`) and open `http://192.168.4.1`.
See [`esp32/README.md`](esp32/README.md) for wiring and setup.

## Documentation

- [`docs/hardware.md`](docs/hardware.md) - board, pin map, connectors, limitations
- [`docs/motor.md`](docs/motor.md) - motor data and the constants derived from it
- [`docs/control.md`](docs/control.md) - FOC implementation, tuning, sampling window
- [`docs/bringup.md`](docs/bringup.md) - ordered test and calibration procedure
- [`docs/esp32.md`](docs/esp32.md) - ESP32 WiFi bridge protocol, timing, and safety design
- [`firmware/tests/README.md`](firmware/tests/README.md) - expected output per test
- [`firmware/foc_esp/README.md`](firmware/foc_esp/README.md) - FOC + ESP32 application details
- [`esp32/README.md`](esp32/README.md) - ESP32 Arduino build, flash, and wiring guide
- [`hardware/README.md`](hardware/README.md) - PCB files and board notes

## Current state

- Current-mode FOC at 20 kHz, angle from the AS5600.
- Standalone `foc`: ramps a fixed `iq` and guards at 800 rpm.
- WiFi-controlled `foc_esp`: commanded over half-duplex UART at 230400 baud with dual watchdogs.
- The AS5600 is a bring-up sensor (about 1000 rpm max). Rated speed needs an ABZ
  or SPI absolute encoder.
- The board has no Vbus sense; the 0.3 V/A CSA gain limits measurable current to ±4.6 A.

## License

MIT. See [`LICENSE`](LICENSE).

The bundled ST HAL and CMSIS sources under `firmware/Drivers/` keep their own
ST license terms; see `firmware/Drivers/CMSIS/LICENSE.txt` and the headers in
the HAL sources.
