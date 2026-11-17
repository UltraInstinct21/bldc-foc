# Firmware

One Makefile builds every application from the shared tree in this folder.
Each app produces its own `.elf`, `.bin` and `.hex`, and is flashed on its own.

## Requirements

```sh
sudo apt install gcc-arm-none-eabi dfu-util openocd
```

USB-C is the default for flashing and for the console. An ST-Link on the SWD
header (J5) is the fallback.

## Applications

| App | Purpose |
|---|---|
| `test_00_hello` | Power, clock, UART, control pins |
| `test_01_as5600` | Encoder over I2C1 |
| `test_02_drv8316` | Gate driver in hardware mode (nSLEEP, nFAULT, DRVOFF check) |
| `test_03_pwm` | TIM1 six-channel PWM, 1 µs dead time |
| `test_04_current_sense` | ADC1, DMA, CSA (0.3 V/A) |
| `test_05_open_loop` | Open-loop 6-step with current limiter (the drive verified on the board) |
| `test_06_encoder_cal` | Encoder sign and offset calibration |
| `test_07_esp_link` | USART1 half-duplex ESP32 link verification |
| `test_08_drive_lab` | Interactive drive lab: every modulation, per-phase current, R/L/λ, I/f, sensorless FOC (`tools/lab.py`) |
| `foc` | Final standalone sensored FOC application (default). Experimental while DRV8316 pin 35 floats |
| `foc_esp` | Sensored FOC + ESP32 WiFi telemetry and control (same caveat) |

## Build and flash

```sh
make APP=test_00_hello          # build one app
make APP=test_00_hello flash    # build and flash (USB-C DFU if seen, else ST-Link)
make APP=test_00_hello flash-dfu  # force USB DFU
make APP=test_00_hello flash-swd  # force ST-Link/SWD
make APP=foc USER_DEFS="-DFOC_VBUS=9.0f"   # extra -D options
make APP=test_07_esp_link flash # build and flash link test
make APP=foc_esp flash          # build and flash FOC + ESP app
make apps                       # build all apps
make APP=foc size               # flash and RAM usage
make list                       # list app names
make clean                      # remove build/
```

`APP` defaults to `foc`.

Artifacts are written to `build/<app>/`:

```
build/foc/foc.elf
build/foc/foc.bin
build/foc/foc.hex
build/foc/foc.map
```

The build directory is ignored by git.

## Toolchain and flags

The compiler is set by `CROSS`, default `arm-none-eabi-`. The target flags
are:

```
-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
-DSTM32G431xx -DUSE_HAL_DRIVER
-O2 -g3 -Wall -Wextra
-ffunction-sections -fdata-sections -MMD -MP
```

Linking uses `--specs=nano.specs --specs=nosys.specs`, garbage collection,
and `-Wl,-u,_printf_float` so `%f` works with newlib-nano.

The linker script is `common/STM32G431CBTX_FLASH.ld`. It limits flash to
126 KB because the last 2 KB page holds the encoder calibration record.

## Flashing

`make flash` runs `tools/flash.py`, which uses USB-C when it can see the
board there and ST-Link otherwise.

**USB-C (default, verified on the board).** The board shows up on USB in one
of two ways: as a running app, a serial port named
`/dev/serial/by-id/usb-Custom_STM32G431_<app>_*-if00`, or in the ST ROM
bootloader, as `0483:df11`. The script sends `dfu` to the running app, which
turns the bridge off and reboots into the bootloader. It then runs
`dfu-util -a 0 -s 0x08000000:leave -D <bin>` and waits for the new app.
Close any terminal on the port first.

- A blank chip, or an app that crashes before USB starts, does not show up.
  Jumper BOOT0 (pin 45, PB8) to 3V3, replug, `make ... flash-dfu`, then
  remove the jumper. The reset button SW4 does not work, so replug instead.
- dfu-util's `Error during download get_status` and
  `Invalid DFU suffix signature` are normal.
- Flash without sudo by adding a udev rule:
  `SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="df11", MODE="0666"`.
- If the DFU device drops: `sudo systemctl stop fwupd`.

**ST-Link (fallback).** OpenOCD:

```
openocd -f interface/stlink.cfg -f target/stm32g4x.cfg \
        -c "program <elf> verify reset exit"
```

Override the interface or target if your probe differs:

```sh
make APP=foc flash-swd OOCD_INTERFACE=interface/stlink-v2.cfg
```

J5 has no reset wire. If the probe cannot connect, hold NRST to GND while
it connects.

## Console

Open the USB serial port, e.g.
`picocom /dev/serial/by-id/usb-Custom_STM32G431_*-if00` (any baud rate).
Output goes there while the port is open, and to USART1 TX on J7 pin 1
(115200 8N1) otherwise. Opening the port prints the app name. Commands:
`dfu` reboots into the bootloader, and `Ctrl-C` or `ESC` turns the bridge off.

## TinyUSB

`Middlewares/tinyusb/` vendors TinyUSB 0.17.0 (`src/` only, MIT), the same
version the board was brought up with. Only the CDC device class and the
STM32 FSDEV port are compiled.

## Adding an application

1. Create `tests/<name>/main.c` (or a dedicated folder).
2. Add `<name>` to `APPS` in the Makefile.
3. If the app needs FOC modules, add them to that app's `APP_EXTRA` block.

Every app links `common/`, TinyUSB and the HAL sources unconditionally. The board
helpers are described in [`common/README.md`](common/README.md).

## Documentation

- [`../docs/hardware.md`](../docs/hardware.md) - board and pin map
- [`../docs/motor.md`](../docs/motor.md) - motor constants
- [`../docs/trapezoidal.md`](../docs/trapezoidal.md) - six-step commutation (test_05)
- [`../docs/control.md`](../docs/control.md) - control loop and tuning
- [`../docs/bringup.md`](../docs/bringup.md) - test procedure
- [`../docs/esp32.md`](../docs/esp32.md) - ESP32 WiFi bridge interface and protocol
- [`tests/README.md`](tests/README.md) - expected output per test
- [`foc/README.md`](foc/README.md) - standalone FOC application
- [`foc_esp/README.md`](foc_esp/README.md) - FOC + ESP32 WiFi application
