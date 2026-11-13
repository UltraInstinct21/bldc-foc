# Hardware

Board revision **v6**: `bldc_control_final_v6`. KiCad sources are in
[`hardware/`](../hardware/).

## Components

| Part | Role |
|---|---|
| STM32G431CBT6 | MCU, LQFP-48, 170 MHz, 128 KB flash, 32 KB RAM |
| DRV8316R | Three-phase gate driver with integrated FETs, SPI, CSA |
| AS5600 | Magnetic absolute encoder, I2C, external to the board |
| LMR36510 | 24 V to 5 V buck |
| TPS2116 | 5 V power mux (board supply / USB) |
| TPS7A45 | 3.3 V LDO |

The motor phases are driven by the DRV8316R in 6x PWM mode. Current is
measured by three low-side shunts and the internal current-sense amplifiers
(SOA, SOB, SOC). There is no Vbus divider and no external current-sense
amplifier.

## Pin map

Verified against the KiCad PCB.

| Function | Pin | AF | Notes |
|---|---|---|---|
| USART1_TX | PB6 | 7 | Debug console, J7 pin 1 |
| USART1_RX | PB7 | 7 | Not used by the firmware; shared with I2C1_SDA |
| I2C1_SCL | PA15 | 4 | J6 pin 2, AS5600 |
| I2C1_SDA | PB7 | 4 | J7 pin 2, AS5600 |
| SPI1_SCK | PB3 | 5 | DRV8316 |
| SPI1_MISO | PB4 | 5 | DRV8316 |
| SPI1_MOSI | PB5 | 5 | DRV8316 |
| DRV nSCS | PA4 | GPIO | Software chip select |
| DRV nSLEEP | PC13 | GPIO | Idle low (driver asleep) |
| DRV DRVOFF | PA7 | GPIO | Idle high (outputs Hi-Z) |
| DRV nFAULT | PA6 | GPIO | Input, pull-up, active low |
| TIM1_CH1 | PA8 | 6 | INHC, phase C |
| TIM1_CH2 | PA9 | 6 | INHB, phase B |
| TIM1_CH3 | PA10 | 6 | INHA, phase A |
| TIM1_CH1N | PB13 | 6 | INLC |
| TIM1_CH2N | PB14 | 6 | INLB |
| TIM1_CH3N | PB15 | 4 | INLA |
| ADC1_IN1 | PA0 | analog | SOA |
| ADC1_IN2 | PA1 | analog | SOB |
| ADC1_IN3 | PA2 | analog | SOC |
| SWDIO | PA13 | - | J5 |
| SWCLK | PA14 | - | J5 |

Two alternate-function details are easy to get wrong:

- SPI1 on PB3/PB4/PB5 is **AF5**. AF6 on those pins belongs to SPI3.
- TIM1_CH3N on PB15 is **AF4**, not AF6.

## Connectors

| Connector | Pins | Use |
|---|---|---|
| J1 | 1 | 24 V input |
| J2 | 3 | Motor phases C, B, A |
| J5 | 4 | 1 = 3V3, 2 = SWDIO, 3 = SWCLK, 4 = GND |
| J6 | 3 | 1 = 3V3, 2 = PA15, 3 = GND (encoder) |
| J7 | 2 | 1 = PB6 (USART1_TX), 2 = PB7 (USART1_RX / I2C1_SDA) |

J7 has no ground pin. Use J6 pin 3 for the encoder ground.

## Power

The 24 V input is fused and protected before the buck. The 5 V rail is muxed
between the buck and USB-C, then regulated to 3.3 V. The DRV8316R runs from
the protected 24 V rail.

## Known limitations

- **PB10/PB11 are labelled `I2C1_SCL` / `I2C1_SDA` on the schematic but have
  no I2C function on STM32G431CB.** Do not use them for the encoder.
- **No I2C pull-ups.** The AS5600 module must provide them, or fit 4.7 k
  from SCL and SDA to 3V3.
- **No Vbus sense.** `FOC_VBUS` is a compile-time constant.
- **No RC filters on SOx.** Current sampling relies on the DRV8316 CSA
  settling time. Add 100 ohm + 1 nF at the ADC pins if noise is a problem,
  then revisit the sampling window in [`control.md`](control.md).
- **J7 has no ground pin.**
- **BOOT0 floats.** Fit a pull-down if the board ever boots unpredictably.
- **NRST is tied to 3V3.** The reset button only works if that link is
  removed or reworked.
- **USB-C CC pins are pulled to 3V3**, so the board does not negotiate USB
  power correctly.
- **The 24 V bulk capacitor voltage rating has not been verified.**
