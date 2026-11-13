# Bring-up

Do the steps in order. Do not skip to the FOC app before the component tests
pass.

## Before power

1. Inspect the board for solder bridges, especially around the DRV8316R and
   the shunt traces.
2. Verify the 3.3 V rail with the board unpowered from the motor supply, then
   with the supply current-limited to about 100 mA.
3. Confirm the debug console on J7 pin 1 (PB6) at 115200 8N1.
4. Connect the motor to J2 only when a test needs it.

The firmware keeps the bridge safe at boot: nSLEEP low, DRVOFF high, nSCS
high. No test enables the power stage unless it says so.

## Equipment

- ST-Link and OpenOCD
- Current-limited 24 V supply
- Oscilloscope for the PWM and current-sense tests
- USB-UART adapter for the console
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
| 1 | `make APP=test_00_hello flash` | Banner and 1 Hz status at 170 MHz |
| 2 | `make APP=test_02_drv8316_spi flash` | `rc=0`, `CTRL_2 = 0x78` |
| 3 | `make APP=test_01_as5600 flash` | Magnet detected, RAW sweeps 0 to 4095 per turn |
| 4 | `make APP=test_03_pwm flash` | 20 kHz, 120 degrees, about 750 ns dead time |
| 5 | `make APP=test_04_current_sense flash` | About 1.65 V and 0 A on all phases |
| 6 | `make APP=test_05_open_loop flash` | Rotor aligns, then turns to 20 Hz electrical |
| 7 | `make APP=test_06_encoder_cal flash` | `rc=0`, sign and offset printed, valid set |
| 8 | `make APP=foc flash` | Closed-loop run |

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

Use 1 percent modulation (`0.01`). At standstill the current is
`I = mod * Vbus / Rph`, so 1 percent is about 2.2 A for this 0.109 ohm motor.
`FOC_ApplyOpenLoopVector()` clamps to 2 percent as a safety net.

Return codes from `Encoder_Calibrate()`:

| Code | Meaning |
|---|---|
| 0 | Success, record saved |
| -1 | Invalid callback |
| -2 | AS5600 read failed |
| -3 | Rotor did not follow the vector |

For -3, check the phase wiring and the magnet, then try a slightly larger
modulation up to 0.02.

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
- Test 03 and test 05 default to DRVOFF high, so the power stage stays Hi-Z.
  Only `PWM_TEST_ENABLE_OUTPUT` in test 03 changes that.
- Use a current-limited supply for the first runs of any test that enables
  the bridge.
