# bldc-foc

Sensored field-oriented control firmware for a custom 24 V BLDC drive:

- STM32G431CBT6 (170 MHz, LQFP-48)
- DRV8316R gate driver, 6x PWM over SPI, three low-side shunts
- AS5600 magnetic encoder on I2C1
- 24 V outrunner, 7 pole pairs, 5 Arms, 2000 rpm

The firmware is split into independent test applications, one per board
function, plus the final FOC application. Each app links the same board layer
and is flashed on its own, so a failure points at one component.

## Layout

| Path | Contents |
|---|---|
| `firmware/` | Build system and all source code |
| `firmware/common/` | Clock, GPIO, pin AFs, interrupts, UART console |
| `firmware/foc/` | Final FOC app and the FOC/encoder/driver modules |
| `firmware/tests/` | Seven independent hardware test apps |
| `firmware/Drivers/` | ST HAL and CMSIS |
| `hardware/` | KiCad schematic and PCB for board revision v6 |
| `docs/` | Hardware, motor, control and bring-up documentation |

## Quick start

Requirements: `arm-none-eabi-gcc`, OpenOCD, and an ST-Link.

```sh
cd firmware

make APP=test_00_hello flash    # first board check
make APP=foc flash              # final application
make apps                       # build every app
make list                       # list app names
```

Full build and flash details are in [`firmware/README.md`](firmware/README.md).

## Documentation

- [`docs/hardware.md`](docs/hardware.md) - board, pin map, connectors, limitations
- [`docs/motor.md`](docs/motor.md) - motor data and the constants derived from it
- [`docs/control.md`](docs/control.md) - FOC implementation, tuning, sampling window
- [`docs/bringup.md`](docs/bringup.md) - ordered test and calibration procedure
- [`firmware/tests/README.md`](firmware/tests/README.md) - expected output per test
- [`hardware/README.md`](hardware/README.md) - PCB files and board notes

## Current state

- Current-mode FOC at 20 kHz, angle from the AS5600.
- No speed or position loop; `foc` ramps a fixed `iq` and guards at 800 rpm.
- The AS5600 is a bring-up sensor (about 1000 rpm). Rated speed needs an ABZ
  or SPI absolute encoder.
- The board has no Vbus sense and no RC filters on the current-sense outputs.

## License

MIT. See [`LICENSE`](LICENSE).

The bundled ST HAL and CMSIS sources under `firmware/Drivers/` keep their own
ST license terms; see `firmware/Drivers/CMSIS/LICENSE.txt` and the headers in
the HAL sources.
