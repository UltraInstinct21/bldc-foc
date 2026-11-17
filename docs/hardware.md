# Hardware

Board revision **v7**. v7 only renames v6: the schematic and layout are
identical, and the KiCad files still carry the `bldc_control_final_v6` name.
KiCad sources are in [`hardware/`](../hardware/).

Everything marked *measured* below comes from the board bring-up on
2026-11-17 (`varun_foc_context_README.md` and its firmware log).

## Components

| Part | Role |
|---|---|
| STM32G431CBT6 | MCU, LQFP-48, 170 MHz, 128 KB flash, 32 KB RAM |
| DRV8316T | Three-phase driver with integrated FETs, hardware-strap variant, CSA. The BOM and symbol say `DRV8316RRGFR` (SPI variant), but the T part is fitted and the pins are wired as straps |
| AS5600 | Magnetic absolute encoder, I2C or analog OUT, external to the board |
| LMR36510 | 24 V to 5 V buck |
| TPS2116 | 5 V power mux (board supply / USB) |
| TPS7A45 | 3.3 V LDO |

The motor phases are driven by the DRV8316T in 6x PWM mode. Current is
measured by the integrated low-side current-sense amplifiers (SOA, SOB, SOC).
No SPI bus goes to the driver. Pin straps set the whole configuration, and
there is no Vbus divider.

## DRV8316T straps (as built)

Straps are sampled at power-up only, so a change needs a full power cycle.

| Pin | Strap | Level | Effect | Firmware consequence |
|---|---|---|---|---|
| 33 MODE | R8 0 Ω to GND | 1 | 6x PWM, SOx available | MCU must supply complementary PWM **and the dead time** |
| 34 SLEW | R7 47 kΩ to AVDD | 3 | 125 V/µs | TIM1 dead time 1.0 µs (`PWM_DEADTIME = 0x80 \| 21`) |
| 35 OCP/SR | **open (floating)** | Hi-Z (3) | **ASR + AAR active demagnetisation on**, OCP 16 A | All-phase-switching modulation passes almost no current, see below |
| 36 GAIN | open | Hi-Z | 0.3 V/A | `DRV8316_CSA_V_PER_A = 0.3`; ADC saturates at ±5.4 A |
| 37 VREF/ILIM | 3V3 | — | CSA bias VREF/2 (1.64 V measured) | Ratiometric with the ADC |
| 24 VSEL_BK | GND | 1 | Internal buck 3.3 V, unused rail | none |

### Driver behaviour the firmware relies on (measured)

- **6x PWM truth table per phase:** INH=0/INL=0 → Hi-Z, 1/0 → high,
  0/1 → low, 1/1 → Hi-Z. The driver inserts no dead time.
- **DRVOFF high pulls nFAULT low** (datasheet §8.4.2). The idle state is
  therefore **DRVOFF low + all six INx low** (TIM1 MOE = 0, gate pins pulled
  down). DRVOFF high is an emergency kill only. An earlier firmware idled
  with DRVOFF high, so every "is the driver OK?" check failed.
- **nSLEEP:** a 20–40 µs low pulse clears a latched fault. Longer than about
  120 µs puts the driver to sleep. After a wake, nFAULT is low for about
  1.8 ms, and tWAKE is 1 ms. The firmware uses a 30 µs pulse, waits 2 ms
  after a clear and 5 ms after a wake, and debounces nFAULT over 2 ms.
- **No VM:** with no VIN on J1, the chip's ESD diodes clamp nFAULT low and it
  ignores nSLEEP.
- **CSA:** `V_SOx = VREF/2 + 0.3 V/A × i`, where `i` is positive for current
  flowing out of the phase into the motor. SOx is valid only while that
  phase's low side is on. It reads 0 V while the driver sleeps.
- **Pin 35 floating (ASR/AAR on):** the bridge passes almost no current when
  all three phases PWM. Current flows when at least one phase's low side is
  held on with no edges, which is why 6-step works. The firmware therefore
  uses 6-step or bottom-clamped DPWM, never plain sine or SVPWM. The proposed
  fix is to wire pin 35 to GND and power-cycle; it has not been done. Bridging
  35 to 36 is harmless if both go to GND, but pin 37 is 3V3, so a 36–37 bridge
  shorts the rail. See [`control.md`](control.md#modulation).

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
| USB_DM / USB_DP | PA11 / PA12 | - | USB-C console and DFU flashing (HSI48 + CRS, no crystal) |
| BOOT0 | PB8 (pin 45) | - | No header and no pull resistor; jumper to 3V3 for the ROM bootloader |

Alternate-function note:
- TIM1_CH3N on PB15 is **AF4**, not AF6.

## Connectors

| Connector | Type | Pins | Pinout | Use |
|---|---|---|---|---|
| J1 | Terminal Block 5.0mm | 2 | 1 = VIN (12–24 V), 2 = PGND | Main motor DC power input |
| J2 | Terminal Block 5.0mm | 3 | 1 = Phase C, 2 = Phase B, 3 = Phase A | BLDC motor phase outputs |
| J3 | USB-C Receptacle | - | VBUS, D-, D+, GND | 5 V backup supply, **USB console and DFU flashing (default)** |
| J5 | Pin Header 2.54mm | 4 | 1 = 3V3, 2 = SWDIO, 3 = SWCLK, 4 = PGND | ST-Link SWD programming (fallback, no reset wire) |
| J6 | Pin Header 2.54mm | 3 | 1 = 3V3, 2 = PA15 (I2C1_SCL), 3 = PGND | AS5600 encoder header |
| J7 | Pin Header 2.54mm | 2 | 1 = PB6 (USART1_TX), 2 = PB7 (I2C1_SDA) | ESP32 link & encoder SDA |

## Power Architecture

```
12–24 V DC (J1) ──► F1 PTC ──► Q1 reverse-polarity P-FET ──► VIN_Prot ──► DRV8316T VM
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

8. **ESP32 ADC encoder option:**
   The `foc_esp` application can sample AS5600 OUT through a 10 kΩ/100 kΩ
   divider on ESP32 GPIO34 (ADC1) and send the resulting 0..4095 angle in
   each UART command. The analog path retains the AS5600 output filter delay;
   use the standalone `foc` I2C path when low-latency, high-speed FOC is
   required.

9. **Unconnected Schematic Pins (PB10 / PB11):**
   Pins 22 (PB10) and 25 (PB11) are labeled `/I2C1_SCL` and `/I2C1_SDA` on the schematic, but PB10/PB11 do not support I2C1 on STM32G431 and are completely unrouted on the PCB. Do not use them.

10. **Reset button does nothing:**
   SW4 connects NRST to 3.3 V rather than GND, so pressing it does not
   reset the chip. J5 has no reset wire either. To reset, unplug and replug
   the board, short NRST to GND, or let the firmware reboot itself (the
   USB `dfu` command). Fix: rewire SW4 to GND.

11. **USB-C CC resistors are pull-ups (Rp), not Rd:**
    R12/R13 pull CC1/CC2 to 3.3 V rather than 5.1 kΩ to GND. With a C-to-C
    cable and no VIN, the host may never switch VBUS on. A C-to-A cable, or
    VIN present, avoids it. The data lines are unaffected. Fix: 5.1 kΩ to GND.

12. **BOOT0 has no header or pull resistor:**
    BOOT0 is PB8, pin 45; the net is mislabelled `/PB9`, and R1 sits on PB9.
    A blank chip does not appear on USB. To reach the ROM DFU bootloader,
    jumper pin 45 to 3V3 and replug. Once any app from this repo is running,
    its `dfu` command reboots into the bootloader, so no jumper is needed. If
    the bootloader ignores USB, also tie USART1_RX (PB7, J7.2) high: the ROM
    bootloader locks onto the first interface it sees activity on (AN2606).
    A pulled-up AS5600 SDA already does that. Do **not** tie PA10 high
    instead: PA10 is INHA. Stop `fwupd` while flashing if the DFU device drops.

13. **USB-C connector joints:**
    On the bring-up board the connector had to be reflowed. Before that, the
    board disconnected at random. One `error -71` at plug-in is benign.

14. **With VIN present, the board survives a USB unplug** (buck → mux). If
    unplugging USB resets the MCU, VIN is not reaching the board.
