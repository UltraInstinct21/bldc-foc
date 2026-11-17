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
| Gate driver | DRV8316T (BOM says DRV8316RRGFR), integrated FETs, 6x PWM, hardware strap configuration, no SPI |
| Current sense | Integrated CSA, 0.3 V/A, VREF = 3.3 V, 1.64 V at 0 A (measured), ADC saturates at ±5.4 A |
| MCU | STM32G431CBT6, LQFP-48, 170 MHz, 128 KB flash, 32 KB RAM |
| Power regulators | LMR36510 buck (24 V → 5 V), TPS2116 mux, TPS7A4533 LDO (5 V → 3.3 V) |
| Encoder interface | I2C1 on PA15 (SCL) and PB7 (SDA), J6 / J7 |
| Debug / link | USB CDC console on J3 (default); USART1 on J7 (PB6 TX, PB7 RX) |
| Programming | USB DFU on J3 (default); SWD on J5 |
| PCB | 2-layer, Rev v7 (a file rename of v6; files still named `_v6`) |

## Connectors

| Connector | Pins | Signal | Description |
|---|---|---|---|
| J1 | 2 | 12–24 V / GND | Motor supply input |
| J2 | 3 | ph_C / ph_B / ph_A | Motor phase outputs |
| J3 | — | USB-C | 5 V backup supply, USB console and DFU flashing (see errata) |
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

The DRV8316T runs directly from the protected input rail. The STM32 and all
logic run from the 3.3 V LDO output.

## DRV8316T hardware straps

The DRV8316T is configured entirely by pin straps. There is no SPI connection
to the driver; the firmware only drives nSLEEP and DRVOFF and reads nFAULT.

| Pin | Strap | Setting | Result |
|---|---|---|---|
| MODE | AGND | — | 6x PWM mode, SOA/SOB/SOC outputs active |
| GAIN | Hi-Z | — | CSA gain 0.3 V/A |
| SLEW | 47 kΩ to AVDD | — | Gate slew rate 125 V/µs |
| OCP/SR (pin 35) | Hi-Z (floating) | — | OCP 16 A, **ASR/AAR active demagnetisation on**. Blocks current when all three phases switch; see errata 9 |
| VREF/ILIM | 3V3 | — | 1.65 V mid-rail offset (1.64 V measured) |

Two more driver rules matter for firmware. DRVOFF high pulls nFAULT low, so
the idle state is DRVOFF low with all INx low. The driver adds no dead time
in 6x PWM mode, so TIM1 inserts 1 µs for the 125 V/µs slew. See
[`../docs/hardware.md`](../docs/hardware.md).

## Known deviations and errata

| # | Item | Impact |
|---|---|---|
| 1 | USB-C CC pull-ups are connected to 3.3 V instead of 5.1 kΩ to GND | A C-to-C host may not switch VBUS on when VIN is absent; use a C-to-A cable. Data (console, DFU) is unaffected |
| 2 | F1 PTC fuse is rated 16 V; input may reach 24 V | The fuse may trip at high current on a 24 V supply; verify thermal behaviour before sustained 24 V operation |
| 3 | SW4 connects NRST to 3.3 V instead of GND | Pressing SW4 has no effect, and J5 has no reset wire. Replug the board, short NRST to GND, or use the firmware's `dfu` reboot |
| 4 | BOOT0 (PB8, pin 45) has no header or pull resistor; its net is mislabelled `/PB9` | The MCU boots from flash. For the ROM DFU bootloader on a blank chip, jumper pin 45 to 3V3 and replug (verified). Later flashes use the `dfu` serial command, so no jumper is needed |
| 5 | DRV8316T internal buck (SW_BK / FB_BK) is unused | Only an RC network is fitted; the internal buck is disabled. No functional impact |
| 6 | PB10 and PB11 are labelled I2C1_SCL / I2C1_SDA on the schematic | PB10/PB11 have no I2C1 alternate function on the STM32G431CB. The pads are unconnected on the board; do not use them for the encoder |
| 7 | ADC saturates at about ±5.4 A at 0.3 V/A; rated motor current is 7 A | Full motor torque cannot be measured. Keep peak iq ≤ 4.0 A in firmware. GAIN to GND gives 0.15 V/A (change `DRV8316_CSA_V_PER_A`) |
| 8 | SLEW strap gives 125 V/µs | The driver adds no dead time in 6x mode; TIM1 must insert 1.0 µs |
| 9 | OCP/SR (pin 35) left floating: ASR/AAR on | Almost no current when all three phases PWM (measured). 6-step works; FOC uses bottom-clamped DPWM and only partly works. Fix: wire pin 35 to GND (not AVDD) and power-cycle. Not done yet |
| 10 | USB-C connector joints | Had to be reflowed on the bring-up board; before that USB dropped at random |

## Firmware

The firmware build system and all source code are in `firmware/`. See
`../firmware/README.md` for build instructions, the application list, and
toolchain requirements.

The `foc_esp` application adds a half-duplex UART link to an external ESP32
WiFi bridge. See `../firmware/foc_esp/README.md` and `../docs/esp32.md`.

## Not in this repository

Schematic and PCB PDFs, Gerbers, drill files, the BOM and the motor design
report are kept locally and are covered by `.gitignore`.
