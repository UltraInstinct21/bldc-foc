# Firmware

One Makefile builds every application from the shared tree in this folder.
Each app produces its own `.elf`, `.bin` and `.hex`, and is flashed on its own.

## Requirements

```sh
sudo apt install gcc-arm-none-eabi openocd
```

An ST-Link probe is expected on the SWD header (J5).

## Applications

| App | Purpose |
|---|---|
| `test_00_hello` | Power, clock, UART, control pins |
| `test_01_as5600` | Encoder over I2C1 |
| `test_02_drv8316` | Gate driver in hardware mode (nSLEEP, nFAULT) |
| `test_03_pwm` | TIM1 six-channel PWM |
| `test_04_current_sense` | ADC1, DMA, shunts (0.3 V/A) |
| `test_05_open_loop` | Independent V/f motor spin |
| `test_06_encoder_cal` | Encoder sign and offset calibration |
| `test_07_esp_link` | USART1 half-duplex ESP32 link verification |
| `foc` | Final standalone sensored FOC application (default) |
| `foc_esp` | Sensored FOC + ESP32 WiFi telemetry and control |

## Build and flash

```sh
make APP=test_00_hello          # build one app
make APP=test_00_hello flash    # build and flash
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

The `flash` target calls OpenOCD:

```
openocd -f interface/stlink.cfg -f target/stm32g4x.cfg \
        -c "program <elf> verify reset exit"
```

Override the interface or target if your probe differs:

```sh
make APP=foc flash OOCD_INTERFACE=interface/stlink-v2.cfg
```

## Adding an application

1. Create `tests/<name>/main.c` (or a dedicated folder).
2. Add `<name>` to `APPS` in the Makefile.
3. If the app needs FOC modules, add them to that app's `APP_EXTRA` block.

Every app links `common/` and the HAL sources unconditionally. The board
helpers are described in [`common/README.md`](common/README.md).

## Documentation

- [`../docs/hardware.md`](../docs/hardware.md) - board and pin map
- [`../docs/motor.md`](../docs/motor.md) - motor constants
- [`../docs/control.md`](../docs/control.md) - control loop and tuning
- [`../docs/bringup.md`](../docs/bringup.md) - test procedure
- [`../docs/esp32.md`](../docs/esp32.md) - ESP32 WiFi bridge interface and protocol
- [`tests/README.md`](tests/README.md) - expected output per test
- [`foc/README.md`](foc/README.md) - standalone FOC application
- [`foc_esp/README.md`](foc_esp/README.md) - FOC + ESP32 WiFi application
