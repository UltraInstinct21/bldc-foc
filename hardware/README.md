# BLDC Motor Controller — Board Revision v6

KiCad sources for the 24 V three-phase BLDC drive, board revision v6
(`bldc_control_final_v6`).

| File | Contents |
|---|---|
| `bldc_control_final_v6.kicad_pro` | KiCad project |
| `bldc_control_final_v6.kicad_sch` | Single-sheet schematic |
| `bldc_control_final_v6.kicad_pcb` | 2-layer PCB |

Open the `.kicad_pro` file with KiCad 7 or newer.

## Quick facts

| Item | Value |
|---|---|
| Input voltage | 12 to 24 V DC on J1; USB-C 5 V as alternate supply |
| Motor output | Three phases on J2 (ph_C, ph_B, ph_A) |
| Gate driver | DRV8316CT, integrated FETs, 6x PWM, hardware strap configuration, no SPI |
| Current sense | Integrated CSA, 0.3 V/A, VREF = 3.3 V, linear range ±4.6 A |
| MCU | STM32G431CBT6, LQFP-48, 170 MHz, 128 KB flash, 32 KB RAM |
| Power regulators | LMR36510 buck (24 V → 5 V), TPS2116 mux, TPS7A4533 LDO (5 V → 3.3 V) |
| Encoder interface | I2C1 on PA15 (SCL) and PB7 (SDA), J6 / J7 |
| Debug / link | USART1 on J7 (PB6 TX, PB7 RX) |
| Programming | SWD on J5 |
| PCB | 2-layer, Rev v6 |

## Connectors

| Connector | Pins | Signal | Description |
|---|---|---|---|
| J1 | 2 | 12–24 V / GND | Motor supply input |
| J2 | 3 | ph_C / ph_B / ph_A | Motor phase outputs |
| J3 | — | USB-C | 5 V backup supply and USB data (see errata) |
| J5 | 4 | 3V3 / SWDIO / SWCLK / GND | SWD debug header |
| J6 | 3 | 3V3 / OUT (PA15) / GND | Encoder supply and SCL; also OUT pin |
| J7 | 2 | PB6 (USART1_TX) / PB7 (USART1_RX) | UART console and ESP32 link |

J7 has no ground pin. Use J5 pin 4 or J6 pin 3 as the signal ground.
J6 pin 2 connects to PA15, which carries I2C1_SCL (AF4) for the AS5600.

## Power tree

```
12–24 V input (J1)
    └── F1 PTC fuse
        └── LMR36510 buck  →  5 V
                               └── TPS2116 mux  ←  USB-C 5 V (J3)
                                       └── TPS7A4533 LDO  →  3.3 V
```

The DRV8316CT runs directly from the protected input rail. The STM32 and all
logic run from the 3.3 V LDO output.

## DRV8316CT hardware straps

The DRV8316CT is configured entirely by pin straps. There is no SPI connection
to the driver; the firmware only drives nSLEEP and DRVOFF and reads nFAULT.

| Pin | Strap | Setting | Result |
|---|---|---|---|
| MODE | AGND | — | 6x PWM mode, SOA/SOB/SOC outputs active |
| GAIN | Hi-Z | — | CSA gain 0.3 V/A |
| SLEW | 47 kΩ to AVDD | — | Gate slew rate 125 V/µs |
| OCP/SR | Hi-Z | — | OCP threshold 16 A, active demagnetization |
| VREF/ILIM | 3V3 | — | 1.65 V mid-rail offset, ±4.6 A linear CSA range |

## Known deviations and errata

| # | Item | Impact |
|---|---|---|
| 1 | USB-C CC pull-ups are connected to 3.3 V instead of 5.1 kΩ to GND | USB-C power negotiation fails; the board accepts 5 V but cannot negotiate higher current from a compliant charger |
| 2 | F1 PTC fuse is rated 16 V; input may reach 24 V | The fuse may trip at high current on a 24 V supply; verify thermal behaviour before sustained 24 V operation |
| 3 | SW4 connects NRST to 3.3 V instead of GND | Pressing SW4 has no effect; use the OpenOCD `reset` command or the SWD reset line to reset the MCU |
| 4 | BOOT0 is unconnected; it floats to logic 0 via the internal pull-down | The MCU always boots from flash; DFU mode via BOOT0 is not accessible without a hardware rework |
| 5 | DRV8316CT internal buck (SW_BK / FB_BK) is unused | Only an RC network is fitted; the internal buck is disabled. No functional impact |
| 6 | PB10 and PB11 are labelled I2C1_SCL / I2C1_SDA on the schematic | PB10/PB11 have no I2C1 alternate function on the STM32G431CB. The pads are unconnected on the board; do not use them for the encoder |
| 7 | CSA linear range is ±4.6 A; rated motor current is 7 A | Full motor torque cannot be measured accurately. Keep peak iq ≤ 4.0 A in firmware to stay within the measurement window |
| 8 | SLEW strap gives 125 V/µs (not 200 V/µs) | Dead-time margin is slightly reduced relative to the datasheet default. Keep `FOC_VMAX_FRAC` ≤ 0.45 in firmware |

## Firmware

The firmware build system and all source code are in `firmware/`. See
`../firmware/README.md` for build instructions, the application list, and
toolchain requirements.

The `foc_esp` application adds a half-duplex UART link to an external ESP32
WiFi bridge. See `../firmware/foc_esp/README.md` and `../docs/esp32.md`.

## Not in this repository

Schematic and PCB PDFs, Gerbers, drill files, the BOM and the motor design
report are kept locally and are covered by `.gitignore`.
