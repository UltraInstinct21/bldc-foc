# Bring-up

Do the steps in order. Do not skip to the FOC app before the component tests
pass.

## Before power

1. Inspect the board for solder bridges, especially around the DRV8316T
   (pins 33–37 are 0.5 mm pitch) and the USB-C connector, which had to be
   reflowed on the bring-up board.
2. Verify the 3.3 V rail with the board unpowered from the motor supply, then
   with the supply current-limited to about 100 mA.
3. Connect the motor to J2 only when a test needs it.

The firmware keeps the bridge safe at boot: nSLEEP low, **DRVOFF low**, all
six gate pins low (Hi-Z). DRVOFF is not held high at idle because that pulls
nFAULT low. No test enables the power stage unless it says so.

## Flashing and console

USB-C is the default for both:

```sh
cd firmware
make APP=test_00_hello flash      # USB-C DFU if the board is on USB, else ST-Link
make APP=test_00_hello flash-dfu  # force USB DFU
make APP=test_00_hello flash-swd  # force ST-Link/SWD (J5)
picocom /dev/serial/by-id/usb-Custom_STM32G431_*-if00   # console
```

- **First flash of a blank chip:** it does not show up on USB. Jumper BOOT0
  (pin 45, PB8) to 3V3, replug USB (the reset button does not work), run
  `make ... flash-dfu`, then remove the jumper. Or use an ST-Link on J5.
- **Every later flash** needs no jumper. `tools/flash.py` sends `dfu` to the
  running app over its USB serial port. The app turns the bridge off and
  reboots into the ST ROM bootloader (`0483:df11`), dfu-util writes the
  image, and the new app starts. A whole cycle takes about 4 s. Close the
  terminal first, because only one program can hold the port.
- dfu-util's `Error during download get_status` and
  `Invalid DFU suffix signature` are normal. Look for
  `File downloaded successfully`.
- **Crashed app, no USB:** use the BOOT0 jumper again.
- Flashing without sudo: add
  `SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="df11", MODE="0666"`
  to `/etc/udev/rules.d/50-stm32-dfu.rules`. If the DFU device drops, run
  `sudo systemctl stop fwupd`.
- Use a **C-to-A cable** when VIN is off: the board's CC resistors are
  pull-ups, so a C-to-C host may not switch VBUS on.
- The console prints to USB while a terminal has the port open, and to
  USART1 TX (J7 pin 1, 115200 8N1) otherwise. Opening the port prints the app
  name. `Ctrl-C` or `ESC` turns the bridge off in every app.

## Equipment

- USB-C cable (C-to-A preferred), `dfu-util`
- Optional: ST-Link and OpenOCD (SWD fallback), USB-UART adapter (UART console fallback)
- Current-limited supply. Bring-up was done at 9 V; the design bus is 24 V.
- Oscilloscope for the PWM and current-sense tests
- AS5600 module with a diametric magnet

## Encoder wiring

| AS5600 | STM32 pin | Board access |
|---|---|---|
| SCL | PA15 | J6 pin 2 |
| SDA | PB7 | J7 pin 2 |
| VCC | 3V3 | J6 pin 1 |
| GND | GND | J6 pin 3 |

Fit 4.7 k pull-ups from SCL and SDA to 3V3 if the module does not have them.
J7 has no ground pin.

## Test sequence

Run each command from `firmware/`.

| Step | Command | Pass criterion |
|---|---|---|
| 1 | `make APP=test_00_hello flash` | Banner and 1 Hz status at 170 MHz on the USB console |
| 2 | `make APP=test_02_drv8316 flash` | `rc=0`, `DRVOFF check: PASS`, nFAULT = 1 (needs VIN) |
| 3 | `make APP=test_01_as5600 flash` | Magnet detected, RAW sweeps 0 to 4095 per turn |
| 4 | `make APP=test_03_pwm flash` | 20 kHz, one phase clamped low per sector, 1.0 µs dead time |
| 5 | `make APP=test_04_current_sense flash` | About 1.64 V and 0 A on all phases (needs VIN) |
| 6 | `make APP=test_05_open_loop flash` | 6-step: aligns, ramps to 200 Hz electrical, current held at 2 A |
| 7 | `make APP=test_06_encoder_cal flash` | `rc=0`, sign and offset printed, valid set |
| 8 | `make APP=test_07_esp_link flash` | Half-duplex UART verification, echoes frames, 50 Hz synthetic telemetry |
| 9 | `make APP=foc flash` | Closed-loop run. **Experimental** while DRV8316 pin 35 floats, see below |
| 10 | `make APP=foc_esp flash` | Command-driven closed-loop run with ESP32 WiFi web telemetry (same caveat) |

> **DRV8316 pin 35 (OCP/SR) is floating on this board.** That leaves
> ASR/AAR active demagnetisation on, and the bridge passes almost no current
> when all three phases switch. 6-step (step 6) works. The calibration
> vector and FOC use bottom-clamped DPWM, which helped only in some sectors
> during bring-up. To lift the limit, wire pin 35 to GND, power-cycle,
> re-run the raw-duty check in [`control.md`](control.md#modulation), and
> build FOC with `USER_DEFS=-DFOC_USE_SVPWM=1`.

Detailed expected output for each test is in
[`firmware/tests/README.md`](../firmware/tests/README.md).

## Encoder calibration

The calibration sets the electrical offset and the direction sign, then
stores them in the last 2 KB flash page at `0x0801F800`. The linker script
reserves that page, so the record survives an application reflash and is not
erased by normal code.

The rotor must be free during calibration. The procedure applies vectors at
0, 90, 0, 120 and 240 electrical degrees, reads the mechanical angle at each
point, and writes the result.

The default modulation is `0.025`. It cannot be much smaller: the
alignment vector is bottom-clamped, so at 0° only phase A switches, at duty
`1.5 × mod`. The 1 µs dead time swallows any pulse under 1 µs (2 % duty).
The old value of `0.01` produced no current at all. The effective duty is
about `1.5 × mod − 0.02`, which gives:

| mod | Effective duty | 0.109 Ω motor, 24 V | Drone motor, 9 V |
|---|---|---|---|
| 0.025 (default) | ~1.75 % | ~2.6 A | ~0.4 A |
| 0.05 | ~5.5 % | too much | ~0.8–1 A |

`FOC_ApplyOpenLoopVector()` clamps to `FOC_ALIGN_MOD_MAX = 0.03`. On a
higher-resistance motor or a lower supply, raise both, for example
`USER_DEFS="-DAPP_CAL_MOD=0.05f -DCAL_MOD=0.05f -DFOC_ALIGN_MOD_MAX=0.06f"`.
Watch the supply current the first time.

Return codes from `Encoder_Calibrate()`:

| Code | Meaning |
|---|---|
| 0 | Success, record saved |
| -1 | Invalid callback |
| -2 | AS5600 read failed |
| -3 | Rotor did not follow the vector |

For -3, check the phase wiring and the magnet first. Then raise the
modulation as above: the most likely cause is too little effective voltage
after the dead time.

## First closed-loop run

Tunables at the top of `foc/main.c`:

| Macro | Default | Effect |
|---|---|---|
| `APP_IQ_A` | 1.0 A | Target q-axis current, about 0.07 Nm |
| `APP_IQ_RAMP` | 0.5 A/s | Soft start |
| `APP_RPM_LIMIT` | 800 rpm | Drops torque above this speed |

Start with the shaft clamped. Confirm `id` stays near zero and `iq` tracks.
Then release the shaft and run with a small `APP_IQ_A`.

If the motor vibrates or runs away, the encoder calibration is wrong. Re-run
`test_06_encoder_cal`.

## Safety notes

- The motor has 0.109 ohm phase resistance. Small modulation values already
  produce several amps.
- Test 05 and the FOC app turn the motor. Keep the shaft clear.
- Test 03 keeps the DRV asleep, so the power stage stays Hi-Z. Only
  `PWM_TEST_ENABLE_OUTPUT` changes that.
- Test 05 drives the motor at power-up and retries after faults. Its current
  limiter measures the phase whose low side is held on. Supply current is
  roughly duty × phase current, so a PSU reading of 0.3–0.5 A is normal
  while the phase current is about 2 A.
- `Ctrl-C` or `ESC` on the USB console turns the bridge off in any app.
- Use a current-limited supply for the first runs of any test that enables
  the bridge.
