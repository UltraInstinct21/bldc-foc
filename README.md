# bldc-foc

Sensored field-oriented control firmware for a custom 24 V BLDC drive
(board v7, a file rename of v6):

- STM32G431CBT6 (170 MHz, LQFP-48, no crystal)
- DRV8316T gate driver, 6x PWM in hardware mode, integrated current sense (0.3 V/A)
- USB-C console and DFU flashing (default); SWD and UART as fallbacks
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
| `firmware/common/` | Clock, GPIO, pin AFs, interrupts, USB/UART console, ESP32 link |
| `firmware/Middlewares/tinyusb/` | TinyUSB 0.17.0 (USB CDC console) |
| `firmware/tools/flash.py` | Flasher: USB-C DFU, ST-Link fallback |
| `firmware/foc/` | Standalone FOC application (I2C encoder, automatic start) |
| `firmware/foc_esp/` | FOC application with ESP32 half-duplex UART link |
| `firmware/tests/` | Eight independent hardware test apps (`test_00` to `test_07`) |
| `firmware/Drivers/` | ST HAL and CMSIS |
| `esp32/` | Arduino sketch and web app for external ESP32 WiFi bridge |
| `hardware/` | KiCad schematic and PCB for board revision v7 (files named `_v6`) |
| `docs/` | Hardware, motor, control, bring-up, and ESP32 interface documentation |

## Quick start

### STM32 firmware
Requirements: `arm-none-eabi-gcc` and `dfu-util`, plus a USB-C cable
(C-to-A works best). OpenOCD and an ST-Link are an optional fallback.

```sh
cd firmware

make APP=test_00_hello flash    # first board check (USB-C DFU, else ST-Link)
make APP=test_05_open_loop flash # 6-step spin, the drive verified on the board
make APP=test_07_esp_link flash # verify ESP32 link without motor
make APP=foc flash              # standalone FOC app
make APP=foc_esp flash          # FOC + ESP32 WiFi control app
make apps                       # build every app
```

The first flash of a blank chip needs BOOT0 (pin 45) jumpered to 3V3 and a
replug. After that, `make flash` reboots the running app into DFU by itself.
The console is the USB serial port
`/dev/serial/by-id/usb-Custom_STM32G431_*-if00`.
Full build and flash details are in [`firmware/README.md`](firmware/README.md).

### ESP32 WiFi bridge
Flash `esp32/bldc_bridge/bldc_bridge.ino` using the Arduino IDE.
Connect to WiFi **BLDC-FOC** (password `bldcfoc123`) and open `http://192.168.4.1`.
See [`esp32/README.md`](esp32/README.md) for wiring and setup.

## Documentation

- [`docs/hardware.md`](docs/hardware.md) - board, pin map, connectors, limitations
- [`docs/motor.md`](docs/motor.md) - motor data and the constants derived from it
- [`docs/trapezoidal.md`](docs/trapezoidal.md) - six-step commutation: principle, implementation, measurements, tuning
- [`docs/results/`](docs/results/) - measured results: drive lab (pin 35 floating), pin 35/36 rework, 3D-printed motor
- [`docs/control.md`](docs/control.md) - FOC implementation, tuning, sampling window
- [`docs/bringup.md`](docs/bringup.md) - ordered test and calibration procedure
- [`docs/esp32.md`](docs/esp32.md) - ESP32 WiFi bridge protocol, timing, and safety design
- [`firmware/tests/README.md`](firmware/tests/README.md) - expected output per test
- [`firmware/foc_esp/README.md`](firmware/foc_esp/README.md) - FOC + ESP32 application details
- [`esp32/README.md`](esp32/README.md) - ESP32 Arduino build, flash, and wiring guide
- [`hardware/README.md`](hardware/README.md) - PCB files and board notes

## Current state

- Verified on the board: USB enumeration and DFU flashing without a crystal,
  the DRV8316 interface (idle with DRVOFF low), the current sense (1.64 V at
  0 A, 0.3 V/A), and open-loop 6-step (`test_05_open_loop`) up to 200 Hz
  electrical with a 2 A current limit.
- **DRV8316 pin 35 (OCP/SR) is floating.** ASR/AAR is on, and the bridge
  passes almost no current when all three phases switch. FOC and the encoder
  calibration therefore use bottom-clamped DPWM, which only partly worked
  during bring-up. Treat FOC as experimental until pin 35 is tied to GND. See
  [`docs/control.md`](docs/control.md#modulation).
- Current-mode FOC at 20 kHz, angle from the AS5600.
- Standalone `foc`: ramps a fixed `iq` and guards at 800 rpm.
- WiFi-controlled `foc_esp`: commanded over half-duplex UART at 230400 baud with dual watchdogs.
- The AS5600 is a bring-up sensor (about 1000 rpm max). Rated speed needs an ABZ
  or SPI absolute encoder.
- The board has no Vbus sense. At 0.3 V/A the ADC saturates at about ±5.4 A.
- The reset button (SW4) does not work; replug the board instead.

## License

MIT. See [`LICENSE`](LICENSE).

The bundled ST HAL and CMSIS sources under `firmware/Drivers/` keep their own
ST license terms; see `firmware/Drivers/CMSIS/LICENSE.txt` and the headers in
the HAL sources.
