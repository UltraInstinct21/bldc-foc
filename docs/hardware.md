# Hardware

Board revision **v6**: `bldc_control_final_v6`. KiCad sources are in
[`hardware/`](file:///home/sarthi/Projects/bldc-foc/hardware/).

## Components

| Part | Role |
|---|---|
| STM32G431CBT6 | MCU, LQFP-48, 170 MHz, 128 KB flash, 32 KB RAM |
| DRV8316CT | Three-phase gate driver with integrated FETs, hardware-strapped, CSA |
| AS5600 | Magnetic absolute encoder, I2C mode, external to the board |
| LMR36510 | 24 V to 5 V buck |
| TPS2116 | 5 V power mux (board supply / USB) |
| TPS7A45 | 3.3 V LDO |

The motor phases are driven by the DRV8316CT in 6x PWM mode. Current is
measured by three low-side shunts and the internal current-sense amplifiers
(SOA, SOB, SOC). There is no SPI bus connected to the gate driver; configuration
is established entirely by hardware pin straps (MODE grounded via R8 0 Ω, SLEW
pulled to AVDD via R7 47 kΩ, GAIN and OCP/SR floating Hi-Z). There is no Vbus divider
and no external current-sense amplifier.

## Pin map

Verified against `bldc_control_final_v6.kicad_pcb`:

| Function | Pin | AF | Notes |
|---|---|---|---|
| USART1_TX | PB6 | 7 | Debug console / ESP32 half-duplex link, J7 pin 1 |
| I2C1_SDA | PB7 | 4 | AS5600 I2C data, J7 pin 2 |
| I2C1_SCL | PA15 | 4 | AS5600 I2C clock, J6 pin 2 |
| DRV nSLEEP | PC13 | GPIO | Driver sleep control (active high enable) |
| DRV DRVOFF | PA7 | GPIO | Driver gate disable (active high Hi-Z) |
| DRV nFAULT | PA6 | GPIO | Fault indicator input (pull-up, active low) |
| TIM1_CH1 | PA8 | 6 | INHC, phase C |
| TIM1_CH2 | PA9 | 6 | INHB, phase B |
| TIM1_CH3 | PA10 | 6 | INHA, phase A |
| TIM1_CH1N | PB13 | 6 | INLC, phase C low-side |
| TIM1_CH2N | PB14 | 6 | INLB, phase B low-side |
| TIM1_CH3N | PB15 | 4 | INLA, phase A low-side |
| ADC1_IN1 | PA0 | analog | SOA current sense phase A |
| ADC1_IN2 | PA1 | analog | SOB current sense phase B |
| ADC1_IN3 | PA2 | analog | SOC current sense phase C |
| SWDIO | PA13 | - | J5 pin 2 |
| SWCLK | PA14 | - | J5 pin 3 |

Alternate-function note:
- TIM1_CH3N on PB15 is **AF4**, not AF6.

## Connectors

| Connector | Type | Pins | Pinout | Use |
|---|---|---|---|---|
| J1 | Terminal Block 5.0mm | 2 | 1 = VIN (12–24 V), 2 = PGND | Main motor DC power input |
| J2 | Terminal Block 5.0mm | 3 | 1 = Phase C, 2 = Phase B, 3 = Phase A | BLDC motor phase outputs |
| J3 | USB-C Receptacle | - | VBUS, D-, D+, GND | 5 V backup supply and CC pull-ups |
| J5 | Pin Header 2.54mm | 4 | 1 = 3V3, 2 = SWDIO, 3 = SWCLK, 4 = PGND | ST-Link SWD programming |
| J6 | Pin Header 2.54mm | 3 | 1 = 3V3, 2 = PA15 (I2C1_SCL), 3 = PGND | AS5600 encoder header |
| J7 | Pin Header 2.54mm | 2 | 1 = PB6 (USART1_TX), 2 = PB7 (I2C1_SDA) | ESP32 link & encoder SDA |

## Power Architecture

```
12–24 V DC (J1) ──► F1 PTC Fuse ──► DRV8316CT VM power
                         │
                         ▼
                   LMR36510 Buck ──► 5V_b ──┐
                                            ▼
       USB-C 5 V (J3) ─────────────► TPS2116 Mux ──► 5V_syst ──► TPS7A4533 LDO ──► 3V3 Logic
```

## Known Limitations and Hardware Caveats

1. **J7 Has No Ground Pin:**
   J7 provides only PB6 and PB7. An external ESP32 or serial adapter **must** connect its ground wire to J6 pin 3 or J5 pin 4. Without common ground, signals float and cannot be received.

2. **J6.2 (/OUT / PA15) Has No ADC Channel:**
   PA15 on the STM32G431 has no ADC channel. The board cannot sample the analog output of the AS5600. The encoder must be operated via I2C using PA15 as `I2C1_SCL`.

3. **AS5600 Analog/PWM Filter Latency Trap:**
   The AS5600 analog and PWM outputs have a factory default group delay of ~2.2 ms through the internal slow filter. At 800 RPM (7 pole pairs), 2.2 ms introduces ~74° of electrical phase lag, causing FOC torque collapse. Reading digital `RAW_ANGLE` over I2C bypasses this filter entirely with ~40 µs latency.

4. **Split Wiring for AS5600 I2C:**
   Because SCL is on J6.2 (PA15) and SDA is on J7.2 (PB7), the encoder cable harness must split across both J6 and J7:
   - AS5600 VCC ──► J6 pin 1 (3V3)
   - AS5600 SCL ──► J6 pin 2 (PA15)
   - AS5600 GND ──► J6 pin 3 (PGND)
   - AS5600 SDA ──► J7 pin 2 (PB7)

5. **No On-Board I2C Pull-Up Resistors:**
   PA15 and PB7 lack pull-ups on the PCB. The external AS5600 breakout must have 4.7 kΩ pull-ups to 3.3V populated (present on most commercial modules).

6. **Single-Wire Half-Duplex Required for ESP32 Link:**
   Because PB7 is dedicated to I2C1_SDA, USART1 cannot operate in full-duplex mode. PB6 (J7 pin 1) is used as a single-wire half-duplex line (`HDSEL` mode at 230400 baud). An external 4.7 kΩ pull-up to 3.3V and a 1 kΩ series resistor on ESP32 TX (GPIO17) are mandatory on the bench.

7. **ESP32 Power Isolation:**
   Do **NOT** power the ESP32 from the board's 3.3V rail (J6.1 or J5.1). ESP32 WiFi transmission bursts draw 400–500 mA, which will brown out the TPS7A4533 LDO and reset the STM32. Power the ESP32 via its own USB port.

8. **Unconnected Schematic Pins (PB10 / PB11):**
   Pins 22 (PB10) and 25 (PB11) are labeled `/I2C1_SCL` and `/I2C1_SDA` on the schematic, but PB10/PB11 do not support I2C1 on STM32G431 and are completely unrouted on the PCB. Do not use them.

9. **NRST Tied to 3.3 V:**
   Pushbutton SW4 connects NRST to 3.3 V rather than GND. Use SWD debugger reset commands instead of SW4.

10. **USB-C CC Pull-Ups:**
    CC1/CC2 are pulled to 3.3 V rather than 5.1 kΩ to GND, preventing power negotiation with Type-C PD power supplies.
