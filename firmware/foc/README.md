# FOC application

Final application for the board. Build and flash from the parent folder:

```sh
make APP=foc flash
```

## Files

| File | Role |
|---|---|
| `main.c` | Application: init order, calibration, run loop, telemetry |
| `foc.c` / `foc.h` | Clarke, Park, PI, inverse Park, SVPWM, TIM1 output |
| `encoder.c` / `encoder.h` | Angle tracking, speed, offset calibration, flash record |
| `as5600.c` / `as5600.h` | Non-blocking AS5600 driver on I2C1 |
| `drv8316.c` / `drv8316.h` | SPI configuration and fault access |

`foc.c` depends on `encoder.c`. The other modules are independent.

## Boot sequence

1. `Board_Init()`, I2C1, AS5600, `Encoder_Init()`.
2. SPI1, DRV wake, `DRV8316_Init()`. The driver must be configured before any
   PWM is applied because it sets SLEW to 200 V/us.
3. TIM1, ADC trigger, `FOC_Init()`, ADC1.
4. If no valid calibration exists, run `Encoder_Calibrate()` with
   `FOC_ApplyOpenLoopVector` and save the result to flash.
5. Start the TIM1 counter and ADC DMA, measure zero-current offsets.
6. Prime the encoder tracker with a few blocking samples.
7. `FOC_Enable()`, then DRVOFF low.
8. Main loop: ramp `iq`, poll nFAULT and encoder errors, print telemetry.

The encoder is primed before enabling the loop because `FOC_ControlISR()` only
calls `Encoder_Tick()` once the bridge is enabled. Without priming, the first
cycles would use an angle of zero.

## Run parameters

At the top of `main.c`:

| Macro | Default | Meaning |
|---|---|---|
| `APP_IQ_A` | 1.0 A | Target q-axis current, about 0.07 Nm |
| `APP_IQ_RAMP` | 0.5 A/s | Soft start |
| `APP_RPM_LIMIT` | 800 rpm | Drops torque above this speed |
| `APP_CAL_MOD` | 0.01 | Alignment modulation, about 2.2 A |
| `APP_CAL_HOLD_MS` | 400 ms | Settle time per alignment point |
| `APP_OFFSET_SAMPLES` | 500 | Offset samples, 1 ms apart |
| `APP_ENC_ERR_PER_WINDOW` | 20 | I2C errors per 200 ms before stopping |

There is no speed or position loop. `iq` is a torque command. The rpm guard
keeps the AS5600 inside its usable band.

## Calibration reuse

The calibration is stored in the last 2 KB flash page at `0x0801F800`. The
linker script reserves that page. When the record is valid, `main.c` skips
calibration and starts directly. To force a new calibration, flash
`test_06_encoder_cal` or erase that page.

## Stopping conditions

- nFAULT asserts and does not clear.
- The encoder I2C error rate exceeds the limit.
- The encoder produces no valid samples at startup.

On any stop the bridge is disabled and the app idles. Reset to run again.

## Limits

See [`../../docs/control.md`](../../docs/control.md) for the sampling window,
tuning, protections and known limits. In short: the AS5600 is good to about
1000 rpm, there is no Vbus sense, and the current-sense range is nearly fully
used at rated torque.
