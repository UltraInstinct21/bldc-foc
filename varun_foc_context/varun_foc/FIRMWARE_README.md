# BLDC Board v7: Firmware Bring-up Log and Rewrite Guide

This is the firmware companion to `README.md` (the hardware reference for the board). It records what
was done on 2026-11-17 to bring the board up: booting and flashing it, getting the DRV8316 to drive a
motor, open-loop 6-step, an attempt at sinusoidal drive, and a first test of a PCB-stator motor.

It is written so that someone (or another Claude session) can **rewrite all the firmware and tests
from scratch** without repeating the mistakes. Every place where the hardware forced a firmware
decision is called out. So is every problem, the hardware fix proposed for it, and whether that fix
worked.

Status words used throughout:
- **VERIFIED**: measured on the real board.
- **HYPOTHESIS**: best explanation so far, not yet proven.
- **OPEN**: unresolved.

---

## 0. Short version

| Topic | Status |
|---|---|
| USB enumeration, flashing over USB DFU | VERIFIED. First entry needs a BOOT0 jumper; afterwards the firmware's `dfu` command reboots into the ROM bootloader, so no jumper is needed. |
| Clocks without a crystal | VERIFIED. HSI48 + CRS (synced to USB SOF) for USB, HSI16 → PLL → 170 MHz core. |
| DRV8316 nFAULT "stuck low" | VERIFIED as a firmware bug: DRVOFF held high pulls nFAULT low (datasheet §8.4.2). Idle must use DRVOFF low and all INx low. |
| DRV8316 current-sense outputs (SOA/SOB/SOC) | VERIFIED working: 1.64 V at 0 A, 0.3 V/A (GAIN pin open). ADC saturates at about ±5.4 A. |
| Open-loop 6-step on a drone motor at 9 V | VERIFIED up to 200 Hz electrical with a 2 A current limiter. Saved as `six_step/release/six_step_v1.bin`. |
| Sinusoidal / SVPWM drive | **BLOCKED BY HARDWARE**: with all three phases switching, the DRV passes almost no current. HYPOTHESIS: OCP/SR pin 35 is open, which enables ASR/AAR "active demagnetization". Fix proposed: tie pin 35 to GND. **Not yet done or tested.** |
| PCB-stator (coreless) motor | OPEN: the commutation runs, but the rotor does not move at all. See §8. |

---

## 1. Hardware facts that drive the firmware

The full pin map is in `README.md` §7.2. Below is only what the firmware depends on, with what was
**measured**.

### 1.1 MCU: STM32G431CBT6 (LQFP48, 128 KB flash, 32 KB RAM)

- **No crystal.** PF0/PF1 and PC14/PC15 are unconnected. USB needs HSI48 with CRS locked to the USB
  start-of-frame. The core runs from HSI16, optionally through the PLL.
- **USB FS** on PA11 (D−) and PA12 (D+), with no series resistors.
- **BOOT0 = PB8, pin 45.** It has no header and no pull resistor. The net on that pad is labelled
  `/PB9`, and R1 (10 kΩ to GND) sits on PB9, pin 46, not on BOOT0.
- **Reset button SW4 is wired to 3V3 instead of GND,** so it does nothing (`README.md` §7.4). To reset
  the board, unplug and replug it, or short NRST to GND.
- **SWD is on J5** (PA13/PA14) and has no reset wire. It wasn't used yesterday; everything went over
  USB DFU.

### 1.2 Gate-drive and driver pins

| Signal | MCU pin | Timer | AF | DRV8316 pin |
|---|---|---|---|---|
| INHA | PA10 (32) | TIM1_CH3 | 6 | 27 |
| INLA | PB15 (29) | TIM1_CH3N | **4** (not 6) | 28 |
| INHB | PA9 (31) | TIM1_CH2 | 6 | 29 |
| INLB | PB14 (28) | TIM1_CH2N | 6 | 30 |
| INHC | PA8 (30) | TIM1_CH1 | 6 | 31 |
| INLC | PB13 (27) | TIM1_CH1N | 6 | 32 |
| nFAULT | PA6 (14) | input, external pull-up R6; can also be read as ADC2_IN3 | | 22 |
| DRVOFF | PA7 (15) | GPIO output | | |
| nSLEEP | PC13 (2) | GPIO output | | 23 |
| SOA / SOB / SOC | PA0 / PA1 / PA2 | ADC1_IN1 / IN2 / IN3 | | 40 / 39 / 38 |

**The phases are wired in reverse order to the timer channels: A = CH3, B = CH2, C = CH1.** Keep a
`phase → channel` table in the firmware (`phase_ch[3] = {3, 2, 1}`). Don't assume A = CH1.

### 1.3 DRV8316T strap pins, as built

The "T" variant is configured by strap pins, not SPI. Each strap is sampled **at power-up only**, so a
change needs a full power cycle. A sleep/wake cycle may also re-sample it; that wasn't tested.

| Pin | Strap on the board | Level | Effect | Notes |
|---|---|---|---|---|
| 33 MODE | R8, 0 Ω to PGND | Mode 1 | 6x PWM, no current limit, SOx available | **Conflicting reports:** early in the session the user said R8 was unsoldered and open; later they said it is shorted. The measurements (SOx working) fit Mode 1 (shorted). Confirm with a DMM: pin 33 to GND should read about 0 Ω. |
| 34 SLEW | R7, 47 kΩ to AVDD | Level 3 | 125 V/µs | Dead time ≈ 1 µs in firmware. Don't touch. |
| 35 OCP/SR | **open** (no pad) | Hi-Z = level 3 | **ASR + AAR enabled**, OCP 16 A | **Suspected cause of the sine-drive failure** (§6). Proposed fix: tie it to GND = level 1 (ASR/AAR off, OCP 16 A). Do **not** tie it to AVDD (level 4 keeps AAR on). |
| 36 GAIN | open | Hi-Z | 0.3 V/A | Optional: GND gives 0.15 V/A, which doubles the measurable range to about ±10.9 A. **The firmware constant `CSA_GAIN_V_PER_A` must change with it,** or the current limiter lets through 2× the intended current. |
| 37 VREF/ILIM | tied to 3V3 | | CSA reference = 3.3 V | Ratiometric with the ADC (VREF+ = 3V3). In Mode 1 this pin is VREF. |
| 24 VSEL_BK | PGND | | internal buck = 3.3 V | Unused rail |

AGND (pins 2 and 26) and PGND are the same net on this board.

**Soldering warnings for pins 35/36:**
- The pitch is fine. Bridging 35 to 36 is harmless if both go to GND.
- **Pin 37 is 3V3,** so a bridge from 36 to 37 shorts the rail.
- Pin 34 is SLEW; don't bridge it.

### 1.4 DRV8316 behaviour the firmware must respect (VERIFIED unless noted)

- **6x PWM truth table, per phase:**
  - INH = 0, INL = 0 → Hi-Z
  - INH = 1, INL = 0 → high
  - INH = 0, INL = 1 → low
  - INH = 1, INL = 1 → Hi-Z
- **In 6x mode the driver inserts no dead time of its own.** The MCU's TIM1 dead time is the only
  shoot-through protection. 1 µs is used, matching the 125 V/µs slew setting.
- **DRVOFF high makes the DRV pull nFAULT low** (datasheet §8.4.2). This is the most important gotcha.
  If the firmware idles with DRVOFF high, every "is the driver OK?" check fails. **Idle state = DRVOFF
  low + all six INx low (TIM1 MOE = 0, CCER = 0).** The bridge is then Hi-Z by the truth table.
- **nSLEEP:**
  - A 20–40 µs low pulse (tRST) clears a latched fault.
  - After wake, wait at least 1 ms (tWAKE) before use. During that time nFAULT is low for about 1.8 ms
    (measured).
- **Unpowered DRV (no VM):** nFAULT is clamped low by the chip's ESD diodes and ignores nSLEEP. The
  `diag` command tells this case apart by reading nFAULT's voltage on the ADC (§9.6).
- **Current-sense amplifiers:**
  - V_SOx = VREF/2 − G·I_LS, where I_LS is the current into the low-side FET. Measured offset
    1.64 V, G = 0.3 V/A.
  - SOx is **only valid while that phase's low-side FET is on.**
  - At 0.3 V/A and VREF = 3.3 V the ADC rails at about ±5.4 A (2.686 mA per ADC count).
- **The DRV8316 is rated 8 A peak,** so firmware limits must stay well below that.

### 1.5 USB-C / power quirks (from `README.md` and bring-up)

- **The USB-C CC resistors are pull-ups (Rp) instead of pull-downs (Rd)** (`README.md` §5.3). With a
  C-to-C cable and no VIN, the host may never turn on VBUS. A C-to-A cable, or VIN present, avoids it.
- **The USB-C connector needed reflowing.** Before that, the board randomly disconnected (after 4 s,
  5 min, 2 s). After the user resoldered it, the only error seen was one `error -71` at the moment of
  plug-in, which is benign. **VERIFIED fixed.**
- With VIN present, the board stays powered when USB is unplugged (buck → mux), so the MCU doesn't
  reset. If unplugging USB resets the MCU, VIN isn't reaching the board.

---

## 2. Toolchain, libraries and project layout

| Item | Detail |
|---|---|
| Compiler | `gcc-arm-none-eabi` + `libnewlib-arm-none-eabi` (Ubuntu packages) |
| Flasher | `dfu-util` 0.11 (`sudo apt install dfu-util`) |
| udev rule (flash without sudo) | `SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="df11", MODE="0666"` in `/etc/udev/rules.d/50-stm32-dfu.rules` |
| Libraries (git clones in `~/stm32g431-hello/lib`) | TinyUSB **0.17.0**; ARM CMSIS_5 **5.9.0**; ST `cmsis_device_g4` (commit 626ee41) |
| Style | Bare-metal CMSIS register access, no HAL, no RTOS |
| Linker script | `STM32G431CB.ld`: FLASH 128K @ 0x08000000, RAM 22K @ 0x20000000 (SRAM1 + SRAM2; CCM SRAM unused). Has a `.noinit (NOLOAD)` section, needed for the DFU magic. |
| USB IDs | `cafe:4001` (TinyUSB test VID, fine for lab use only). Product strings: "STM32G431 Hello" (hello app) and "STM32G431 6step" (motor app). Scripts find the port by the glob `/dev/serial/by-id/usb-Custom_STM32G431_*-if00`, so **keep the manufacturer string "Custom" and the product prefix "STM32G431"**. |
| ROM bootloader | Shows up as `0483:df11` "STM Device in DFU Mode". alt 0 = internal flash at 0x08000000 (64 × 2 KB). |

```
~/stm32g431-hello/          first bring-up: USB CDC "hello world" + dfu command; owns lib/
~/varun_foc/
  README.md                 hardware reference
  FIRMWARE_README.md        this file
  datasheets/DRV8316.pdf    TI datasheet (SLVSF16B)
  six_step/
    src/main.c              motor firmware (one file, ~1500 lines), current = pcb_motor_v1 source
    src/usb_descriptors.c   TinyUSB descriptors (copied from hello, strings changed)
    src/tusb_config.h       one CDC interface, TX buffer 1024
    Makefile                build/flash; flags AUTOSTART, DRIVE, MOTOR; LIB ?= ~/stm32g431-hello/lib
    STM32G431CB.ld
    flash.py                flash any .bin over DFU, no jumper needed
    phase_test.py           interactive phase / phase-pair test (keys a, b, c ...)
    ser.py                  scripted serial commands + log capture (used for all tests below)
    release/
      six_step_v1.bin/.elf + six_step_v1_src/   VERIFIED 6-step autostart build (16 MHz)
      pcb_motor_v1.bin/.elf + pcb_motor_v1_src/ PCB-motor slow-ramp build (170 MHz)
    wip_sine/               snapshot of the sine work (src/ already contains all of it)
```

Build examples:
```
cd ~/varun_foc/six_step
make clean && make AUTOSTART=1                 # 6-step, autostart, drone-motor profile
make clean && make AUTOSTART=1 MOTOR=pcb       # 6-step, PCB-motor slow ramp profile
make clean && make AUTOSTART=0 DRIVE=sine      # sine drive, waits for "start"
./flash.py build/fw.bin                        # or: make flash
```
**Always run `make clean` when changing AUTOSTART, DRIVE or MOTOR.** They are `-D` flags, and make
doesn't track them.

---

## 3. Step 1 log: getting the board to boot, enumerate and flash

### 3.1 Problem: laptop doesn't see the board at all

- **Symptom:** nothing in `lsusb`, no kernel log events, no "descriptor read error".
- **Cause:** a blank chip. The STM32 only appears on USB when running code enables the USB pull-up.
- **What worked:**
  1. Pull BOOT0 (pin 45, PB8) to 3V3 with a jumper wire.
  2. Reset by unplugging and replugging. The reset button doesn't work, and adding the jumper alone
     does nothing, because BOOT0 is only read at reset.
  3. The ROM bootloader enumerates as `0483:df11`.

### 3.2 Problem: DFU device drops after a few seconds, or doesn't come back after a replug

Things tried or considered:

1. **fwupd** (Ubuntu's firmware updater) probed the DFU device at the moment it appeared:
   `FuPluginDfu failed to parse interface data`. Possible contributor. Workaround:
   `sudo systemctl stop fwupd` while flashing.
2. **Floating UART receive pins in bootloader mode.** The ROM bootloader listens on USB and several
   UART, I2C, SPI and FDCAN interfaces at once, and locks onto the first that shows activity (ST
   AN2606). Noise on a floating RX pin can make it ignore USB.
   - The user tied **BOOT0 (pin 45) and USART1_RX (pin 44, PB7, the J7 UART RX)** to 3V3 for the
     boot. The board then connected at 01:28:38 and `dfu-util -l` worked.
   - The user also had the J7 RX and TX shorted at the time.
   - Caution for the future: PA10, the bootloader's other USART1_RX option, is **INHA**. Don't tie
     PA10 high with VIN and a motor connected.
3. **USB-C plug orientation:** suggested, as a common custom-board fault (only one D+/D− pair wired).
   Not confirmed as the cause.
4. **Intermittent USB-C solder joints:** the board kept dropping (after 4 s, 5 min, 2 s).
   - **What fixed it:** the user resoldered the USB-C connector. **VERIFIED:** stable since then,
     apart from one benign `-71` error at plug-in.

**Hardware recommendations for the next revision:**
- A BOOT0 header or button with a pull-down.
- 10 kΩ pull-ups on the UART RX lines.
- Join both D+/D− pairs on the USB-C connector.
- Rd (5.1 kΩ to GND) on CC1/CC2.
- Rewire the reset button to GND.

### 3.3 First firmware: USB CDC hello world (`~/stm32g431-hello`)

- **Clocks:**
  - Core on HSI16 (the reset default).
  - USB clock: HSI48 on (`RCC_CRRCR_HSI48ON`), `CCIPR.CLK48SEL = 00` (HSI48).
  - CRS: sync source = USB SOF (`CRS_CFGR_SYNCSRC_1`), AUTOTRIMEN + CEN. **VERIFIED:** USB works with
    no crystal.
- **TinyUSB:** device mode, one CDC interface, `OPT_MCU_STM32G4`, `stm32_fsdev` port. The IRQ handlers
  `USB_HP_IRQHandler`, `USB_LP_IRQHandler` and `USBWakeUp_IRQHandler` all call `tud_int_handler(0)`.
  Call `tud_task()` in the main loop.
- **Flashing:** `dfu-util -a 0 -s 0x08000000:leave -D build/fw.bin`. Messages that are **normal** and
  not errors:
  - `Error during download get_status` (exit code 74): the chip jumps to the app before answering.
  - `Invalid DFU suffix signature`.
  - `Cannot open DFU device 30c9:0069`: the laptop's webcam.

  Check for `File downloaded successfully` instead.

### 3.4 Flashing without the jumper: the `dfu` command (VERIFIED)

Every firmware keeps this. Without it, the next flash needs the BOOT0 jumper again.

1. **The serial command `dfu`:**
   - stops the motor;
   - prints a message and waits about 100 ms for USB to send it;
   - writes `0xB007DF11` to a variable in `.noinit` RAM;
   - calls `NVIC_SystemReset()`.
2. **At the very top of `main()`, before any clock setup**, if the magic is present:
   - clear it;
   - enable the SYSCFG clock and set `SYSCFG->MEMRMP = MEM_MODE_0` (system memory at 0);
   - set `SCB->VTOR = 0x1FFF0000`;
   - load MSP from `*(0x1FFF0000)` and jump to `*(0x1FFF0004)`.
3. The host waits for `0483:df11`, then flashes with `:leave`. The new app starts by itself.

`flash.py` and `make flash` both do this automatically: write `"\rdfu\r"` to the app port, wait up to
10 s for DFU, run dfu-util, check for "File downloaded successfully", then wait for the app port. A
whole cycle takes about 4 s.

**Recovery when an app is broken** (crashes before USB, so `dfu` can't run): BOOT0 jumper on, replug,
flash, jumper off.

---

## 4. Step 2 log: open-loop 6-step firmware (drone motor)

The board has **no Hall sensors and no phase-voltage dividers**, so back-EMF zero crossings can't be
detected. Everything below is **open-loop (forced) commutation**: align the rotor, then step the
field at a commanded electrical frequency, like a stepper motor.

### 4.1 PWM design (VERIFIED with pin read-back and motor runs)

- **TIM1, centre-aligned mode 1, 20 kHz:** ARR = f_TIM / (2 · 20 kHz). That's 400 at 16 MHz and
  4250 at 170 MHz.
- **Complementary outputs with 1 µs dead time.**
  - Release v1: DTG = 16 ticks at 16 MHz.
  - Current code: computed from ns with the RM0440 DTG encoding, `deadtime_dtg()`. At 170 MHz it's
    `0x80 | 21`.
- **BDTR: OSSR = OSSI = 1, MOE = 0 at boot.** MOE is set only when driving.
- **Commutation is atomic:** `CR2.CCPC = 1`, so CCMRx.OCxM and CCER bits are preloaded and applied
  together on a `EGR = COMG` event. All three phases change on the same clock edge.
- **Per step, each phase is one of:**
  - **P** = PWM mode 1, CCxE = CCxNE = 1: high side PWM with synchronous-rectifying low side.
  - **L** = "force inactive" (OCxM = 4) with CCxE = CCxNE = 1: CHx = 0, CHxN = 1, low side held on.
  - **Z** = CCxE = CCxNE = 0: pins not driven.
- **GPIO pull-downs on all six gate pins** (plus the DRV's internal input pull-downs) make an
  undriven phase read INH = INL = 0, which is Hi-Z.
- **Step table, {A, B, C}:**

  | Step | A | B | C | Name |
  |---|---|---|---|---|
  | 0 | P | L | Z | A+ B− |
  | 1 | P | Z | L | A+ C− |
  | 2 | Z | P | L | B+ C− |
  | 3 | L | P | Z | B+ A− |
  | 4 | L | Z | P | C+ A− |
  | 5 | Z | L | P | C+ B− |

  Forward = step + 1, reverse = step − 1.
- **Commutation timer TIM6:** 100 kHz tick (10 µs), one interrupt per step, ARR = 100 kHz / (6 · f),
  preloaded so the new period applies next step. f ranges from 0.5 Hz (333 ms per step) to 500 Hz.
- **SysTick at 1 kHz** handles:
  - nFAULT debounce (2 consecutive ms);
  - duty slew (0.05 %/ms);
  - current limiter;
  - ALIGN → RUN transition;
  - frequency ramp.

### 4.2 Measured pin read-back (no motor, 12 V, 20% duty, 20 Hz)

The firmware samples GPIOA/GPIOB IDR in bursts during RUN and reports the % of time each gate pin is
high:
```
  step       INHA INLA INHB INLB INHC INLC   (% of time high)
  0 A+B-     17   79    0  100    0    0
  1 A+C-     17   79    0    0    0  100
  2 B+C-      0    0   17   79    0  100
  3 B+A-      0  100   17   79    0    0
  4 C+A-      0  100    0    0   17   79
  5 C+B-      0    0    0  100   17   79
```
On the PWM phase, INH reads 17% and INL 79% (not 20/80) because of the two 1 µs dead times per
period. The tolerance used in the check is ±8 percentage points.

### 4.3 Problem: "start refused: nFAULT is low and would not clear"

- **First (wrong) diagnosis:** VIN missing. A useful lesson, but not the cause.
- **Real cause, VERIFIED with the `diag` command:** the firmware idled with **DRVOFF high**, and the
  DRV pulls nFAULT low while DRVOFF is high (datasheet §8.4.2).
- **Fix:** idle = DRVOFF low + MOE = 0 + CCER = 0. Afterwards:
  - nFAULT stays at 3.29 V;
  - after a sleep/wake it goes low for 1.8 ms, then high (normal);
  - DRVOFF high → nFAULT low, and releasing DRVOFF brings it straight back high.
- **Also added:** the autostart build retries `start` every 1 s while nFAULT is low (quietly after the
  first message), so the board recovers with no user input.

### 4.4 Driving the drone motor (9 V supply, about 0.5 Ω line-to-line)

- **Problem: violent vibration and about 3+ A at standstill.** An unlimited 20% duty on a 0.5 Ω motor
  gives about 3.6 A of stall current. **Fix: a current limiter using the DRV's CSA.**
  - The phase whose low side is held on (L) carries the full winding current continuously: during
    PWM-on via HS(P) → motor → LS(L), during PWM-off via LS(P) → motor → LS(L). So that phase's SOx
    measures motor current all the time.
  - Each 1 ms: take 8 ADC conversions of the L phase's SOx, convert to |I|, then EMA with α = 0.2.
    Discard the sample if a commutation happened during the burst (sequence counter).
  - `csa_calibrate()` reads the three zero-current offsets with the bridge off, before every start.
    Valid if each is 1.3–2.0 V, which sets `csa_ok`. If CSA isn't OK, duty is capped at 8%.
  - **Limiter:**
    - If i_filt > i_limit: cap = min(applied, cap) − 0.1 %/ms.
    - Otherwise: cap rises 0.02 %/ms, but never more than 2% above the applied duty.
    - Applied duty = min(target, cap).
  - **Trip:** i_filt > min(2·i_limit, I_TRIP_MAX) for 20 ms → stop, then retry after 3 s in
    autostart builds.
- **Bug fixed: limiter wind-up.** The cap started at 50% and fell only 0.1 %/ms, so the motor
  overcurrent-tripped during ALIGN. Now `limiter_reset()` sets the cap to 2% at every start, and the
  cap can't run more than 2% above the applied duty.
- **Bug fixed: OC retry delay wrap.** The retry time was stored in the future and an unsigned
  subtraction wrapped, so the motor restarted immediately. Now a separate `retry_delay` variable is
  used.
- **Results (VERIFIED, 9 V, 2 A limit, pole pairs assumed 7):**

  | Electrical Hz | Applied duty | Phase current avg / peak |
  |---|---|---|
  | 15 | 6% | 0.8 / 1.2 A |
  | 60 | 9.7% | 0.9 / 2.1 A |
  | 100 | 14.5% | 1.7 / 2.7 A |
  | 200 | 24% | 1.7 / 2.9 A |

  Duty rising with speed means back-EMF is tracking, so the rotor stays in sync. The user reported
  that at high speed it ran "smooth af"; the roughness was at low speed (15–60 Hz).
- **"Why does my supply never show more than about 330–500 mA?"** Supply current ≈ duty × phase
  current: the bridge behaves like a buck converter. 24% × 1.7 A ≈ 0.41 A, against 0.33 A measured.
  **Phase current and supply current are different quantities.** Report both, or label which one is
  meant.
- **The user asked for a 10–11 A limit.** Not possible on this hardware:
  - the CSA saturates at about 5.4 A at 0.3 V/A;
  - the DRV8316 is rated 8 A peak.

  The firmware ceiling was raised to `I_LIMIT_MAX = 5 A`, with `I_TRIP_MAX = 5.3 A`.

### 4.5 Release `six_step_v1.bin` (VERIFIED)

- Built with `AUTOSTART=1` on the **16 MHz HSI** clock, before the 170 MHz change.
- At power-up:
  - clear any fault;
  - calibrate the CSA;
  - align 500 ms;
  - start at 4 Hz;
  - ramp with df/dt = 5 Hz/s + 0.1·f, which takes 4 → 200 Hz in about 12 s;
  - duty target 40%, so the limiter (2 A) actually decides the duty;
  - trip at 4 A;
  - auto-retry after a fault or overcurrent.
- Flash with `./flash.py release/six_step_v1.bin`. Logs at 1 Hz with `log`.

---

## 5. Step 3 log: going to 170 MHz (needed for the sine ISR)

Clock-dependent pieces had to be rewritten, because several were hard-coded for 16 MHz:

| Item | 16 MHz version | 170 MHz version |
|---|---|---|
| Clock | HSI16 direct | PWREN; FLASH 4 WS + PRFTEN/ICEN/DCEN; `PWR->CR5.R1MODE = 0` (boost); PLL = HSI16 /4 (PLLM = 3) ×85 /2 (PLLR = 0), PLLREN; AHB /2 during the switch; SW = PLL; wait for SWS; wait > 1 µs; AHB /1 (RM0440 §6.1.5) |
| TIM1 ARR (20 kHz centre-aligned) | 400 | 4250 |
| Dead time 1 µs | DTG = 16 | `deadtime_dtg(1000)` = 0x80 \| 21 |
| `busy_wait_us` | NOP loop, about 4 cycles/iteration (inaccurate) | DWT->CYCCNT. Enable TRCENA + CYCCNTENA at boot. |
| ADC clock | SYSCLK 16 MHz, no prescaler | `ADC12_COMMON->CCR.PRESC = /4` → 42.5 MHz (the maximum is 60 MHz). **Set it before enabling the ADCs.** |
| SOx sampling time | 47.5 cycles | 6.5 cycles (low-impedance source), 0.45 µs per conversion |
| Pin read-back burst | fixed sample counts | scaled with SystemCoreClock |
| TIM6 prescaler | SystemCoreClock / 100 kHz − 1 | same formula (OK) |
| USB | HSI48/CRS | unchanged, independent of the PLL |

**VERIFIED:** 6-step runs identically at 170 MHz. USB, DFU and the DRV interface all work.

**Interrupt priorities, lower number = higher priority:**

| Interrupt | Priority |
|---|---|
| TIM1 update (sine ISR) | 0 |
| TIM6 (commutation) | 1 |
| USB | 2 |
| SysTick | 3 |

---

## 6. Step 4 log: sinusoidal drive, and the DRV problem that blocks it

### 6.1 Sine engine as implemented (`DRIVE=sine` or the `drive sine` command)

- **ISR on the TIM1 update with RCR = 0.** It fires at both the counter valley and the peak. Work is
  done only when `TIM1->CR1.DIR = 1`, meaning the counter just turned around at the **peak**. With PWM
  mode 1 every output is inactive there, so **all three low sides are on and all three SOx are valid.**
- **In the ISR:**
  - Run ADC1 as a 3-conversion sequence: SQR1 L = 2, channels 1, 2, 3. Poll EOC for each.
  - Convert to signed phase currents.
  - Replace the phase that had the **highest duty**, which has the shortest low-side window, with
    −(sum of the other two). This uses Kirchhoff's current law.
  - Compute |I| = sqrt(2/3 · Σi²) and EMA it (α = 0.02, about 2.5 ms) into i_filt.
- **Angle generation:**
  - Phase accumulator, uint32 (2³² = 360°).
  - dphase = f · 2³² / 20000, signed for direction. SysTick updates it from the ramp.
  - 1024-entry int16 sine LUT. B = A − 120°, C = A + 120°.
- **Amplitude:** in sine mode, "duty %" means line-to-line peak voltage as a % of VIN.
  m = d/100 · 2/√3, capped at 1.1547.
- **Modulation:**
  - First version: min-max injection (= SVPWM), off = −(vmax + vmin)/2, duty = 0.5 + 0.5·(v + off).
  - Current version: **bottom-clamped DPWM** (duty_k = 0.5·(v_k − vmin)), so the lowest phase sits at
    0% (low side continuously on). See §6.3 for why.
- **ISR cost:** 823–852 cycles at 170 MHz, about 10% CPU at 20 kHz.
- **Safety additions:**
  - **V/f ceiling:** amplitude ≤ 8% + 0.12 %/Hz · f. This bounds current even if sensing misreads.
  - **Interlock:** amplitude > 10% with i_filt < 0.05 A for 300 ms → trip ("voltage without current
    means the measurement can't be trusted").

### 6.2 Symptom

The sine waveform on the gate pins was correct (CCR values and pin read-back checked), but the
measured current was about 0 A at 40% amplitude. On the drone motor, sine ran at 20 Hz and tracked up
to about 157 Hz, then lost sync. In the sectors where current was missing, the sensing read 0.00 A
while amplitude rose, which ended in an nFAULT at 177 Hz.

### 6.3 Static `raw` tests: fixed duties on all three phases (VERIFIED, motor stalled, 9 V)

`raw A% B% C%` sets fixed complementary PWM on all three phases for about 0.5 s, then free-run
samples all SOx (`scan`). Phase current is the low-side current while each low side conducts.

| Command (A/B/C duty) | Any phase at 0%? | Result |
|---|---|---|
| raw 10 0 0 | yes (B, C) | **current flows**: A about +2.9 A, B and C about −1.6 A each |
| raw 20 0 10 | yes (B) | **current flows**: A about +3.9 A, B about −5 A (ADC saturated) |
| raw 10 0 5, raw 0 10 10, raw 10 0 10 | yes | **current flows**, correct directions |
| raw 20 2 10 | B at 2% (dead time eats its INH pulse; INL still toggles) | **partial current** (about 0.4 A mean) |
| raw 55 45 50, 54 46 50, 58 42 50, 60 40 50 | no | **about 0 A** (only 1–3% of samples above 0.1 A) |
| raw 30 10 20 | no | **about 0 A** |
| raw 100 80 90, raw 98 80 90 | no (top clamp) | **about 0 A** |

In all of these, gate read-back was correct and nFAULT stayed high.

**Rule found:** current flows only when at least one phase's low side is **held on continuously**
(0% duty, no edges). When every phase switches, current is blocked. Dead time alone can't explain
it: 60/40/50 commands the same 20% A–B voltage as 20/0/10 but gives 0 A instead of about 5 A. That's
also why 6-step works: one phase is always held low.

### 6.4 Bottom-clamped DPWM: partial workaround

Clamping the lowest phase at 0% made current flow, but only in **some 60° sectors**. A trace of 160
ISR samples at 100 Hz electrical (`trace 5`, one sample every 250 µs) gave, by sector:
```
clamped phase + current present (I) or absent (.), one char pair per sample:
AI AI AI AI AI AI AI BI B. B. B. B. B. B. B. B. B. B. B. B.
BI CI CI CI CI CI CI CI CI CI CI CI CI CI A. A. A. A. A. A.
A. A. A. A. A. A. A. B. BI B. B. B. B. B. B. B. B. B. B. B.
BI CI CI CI CI CI CI CI CI CI CI CI CI CI AI A. A. A. A. A.
```
- Currents collapse from about 4–5 A to exactly 0 within one 250 µs sample, and stay at 0 for a
  whole sector.
- They recover at certain sector changes.

That looks like a latch inside the DRV that is set and reset by gate-edge or current-zero events. The
motor gets torque for only part of each electrical revolution: rough running, then loss of sync.

### 6.5 Root cause analysis (HYPOTHESIS, not yet proven)

1. **First hypothesis, later withdrawn:** MODE in current-limit mode (Mode 2/4), with pin 37 at 3.3 V
   making the cycle-by-cycle limiter always active. This was based on the early report that R8 was
   open.
   - It was withdrawn because the datasheet says SOx is **not available** in Mode 2/4, yet SOx works
     here.
   - The user then confirmed R8 is shorted (Mode 1).
2. **Current hypothesis:** **OCP/SR (pin 35) is open, which is level 3: ASR and AAR are enabled**
   (datasheet §8.3.12, Table 8-7).
   - With AAR, the DRV turns a conducting low-side FET off when it sees the current near zero.
   - When every phase PWMs, the current starts from zero after each blocked period, so the low side
     that should carry it is turned off → no path → current never builds.
   - A low side held on with **no PWM edges** isn't re-evaluated, which explains why a 0%-duty phase
     works.
3. **Proposed fix:** solder a wire from pin 35 to GND, then power-cycle. That selects level 1: ASR/AAR
   disabled, OCP 16 A.
   - Then re-run the §6.3 `raw` table. Expected: 60/40/50 gives about the same current as 20/0/10.
   - If it does, use normal SVPWM (min-max injection) and keep the 3-shunt sampling at the counter
     peak.
   - If it doesn't, suspect a bad joint on pin 33 MODE, and measure pin 33 to GND.

Until pin 35 is grounded, **only 6-step (or modulation with one phase statically low) works.**

---

## 7. Every software decision caused by this hardware

| # | Hardware fact | Firmware rule |
|---|---|---|
| 1 | No crystal | HSI48 + CRS (SOF sync) for USB; HSI16 (+ PLL) for the core |
| 2 | No BOOT0 header or pull; reset button doesn't work | Keep the `dfu` serial command in **every** build. Jump to 0x1FFF0000 via a `.noinit` magic + reset. Provide `flash.py`. |
| 3 | DRV8316 in 6x PWM mode, no internal dead time | TIM1 complementary outputs with **1 µs dead time** (slew 125 V/µs). Recompute DTG when the clock changes. |
| 4 | Phases wired A = CH3, B = CH2, C = CH1; INLA on PB15 uses **AF4** | Lookup table `phase_ch`; per-pin AF table |
| 5 | DRVOFF high pulls nFAULT low | Idle with DRVOFF **low**, MOE = 0, CCER = 0. Never use DRVOFF as the idle state. |
| 6 | Faults clear with a 20–40 µs nSLEEP pulse; tWAKE 1 ms | `clear_fault()` = 30 µs pulse + 2 ms wait. Wait 5 ms after boot before the first start. |
| 7 | nFAULT is also the "unpowered DRV" indicator | Debounce 2 ms in RUN. The `diag` command reads it as an analog voltage to tell "unpowered" from "real fault". |
| 8 | CSA valid only when that phase's low side is on | 6-step: measure the L phase only, discard samples across a commutation. Sine: sample at the TIM1 counter peak and rebuild the highest-duty phase via KCL. |
| 9 | CSA offset ≈ VREF/2 varies a little | Calibrate the offsets with the bridge off before every start. Reject offsets outside 1.3–2.0 V and cap duty at 8% if CSA is unusable. |
| 10 | CSA gain 0.3 V/A (GAIN open); ADC saturates at about ±5.4 A; DRV is 8 A peak | `I_LIMIT_MAX` 5 A, `I_TRIP_MAX` 5.3 A. **If GAIN is tied to GND, change `CSA_GAIN_V_PER_A` to 0.15** and allow up to about 7 A. |
| 11 | No back-EMF or Hall sensing | Open-loop only: align, then forced commutation with a ramp; current limiter + overcurrent trip; V/f ceiling in sine mode |
| 12 | OCP/SR open → ASR/AAR on (suspected) | Until fixed: 6-step only (one phase statically low). Sine needs pin 35 → GND. |
| 13 | User requirement: no manual typing to start, stop or recover | `AUTOSTART=1` builds: start at power-up, retry every 1 s on nFAULT, 3 s after overcurrent; `diag`, `ph exit` and `drive` restart the motor themselves |
| 14 | Supply current ≠ phase current | Report phase current and label it. The supply sees about duty × I_phase. |
| 15 | User keeps VIN at 9 V | All tuning (duty, limits, top speed) was done at 9 V |

---

## 8. Step 5 log: PCB-stator motor (coreless, windings are PCB traces)

- **User data:**
  - 6-coil stator; the rotor magnet count was not given.
  - The firmware assumes **2 pole pairs** (4 magnets). If it's actually 8 magnets, every rpm shown is
    2× too high.
- **Build:** `make AUTOSTART=1 MOTOR=pcb` = `release/pcb_motor_v1.bin`.
  - Duty 30%, align 1 s, start at 1 Hz electrical (30 rpm).
  - **Constant +10 rpm/s** (accel = 10·pp/60 Hz/s, `accel_prop` = 0) up to 1000 rpm (33.3 Hz).
  - The ramp was VERIFIED by step count: 43 → 1000 rpm in about 98 s.
- **Winding measurement** (each pair held 2 s at 30% duty, 170 MHz build): 0.74 A reading on AB, BC
  and CA alike, so all three phases conduct.
  - At 50% duty, stepping at 3 steps/s, the reading was 0.96 A.
  - The old 16 MHz build read only 0.12–0.13 A at 20% (the first reading was taken mid-ramp, at 18%).
    **The builds disagree** (see the caveat below).
- **Result: the rotor doesn't move at all,** not even at 3 steps/s at 50% duty. That's not a sync
  problem: a rotor that can't follow still twitches.
- **Candidate causes, OPEN:**
  1. Too little torque. PCB coils are weak, so several amps may be needed. The supply may be in
     current limit: the user reported the PSU never showed more than about 0.5 A. Check its CC light.
  2. The magnet air gap is too large.
  3. Coils within a phase are routed in opposite directions, so their fields cancel: current flows
     but there's no torque.
  4. **Measurement caveat (HYPOTHESIS):** a coreless PCB coil has very low inductance, so L/R is much
     shorter than the 50 µs PWM period. The current is then **pulsed**, not continuous.
     - The 6-step current reading takes a short ADC burst once per 1 ms SysTick, and 1 ms is exactly
       20 PWM periods, so **every sample lands at the same PWM phase**.
     - At 170 MHz the burst is about 3.6 µs, so it likely reads the pulse **peak** (about 9 V / R).
     - At 16 MHz the burst is about 30 µs, so it reads something closer to an average.
     - That would explain 0.74 vs 0.12 A, and the reading barely changing with duty.
     - The real average current is probably about duty × peak, which is small, and that would fit
       "no torque".
     - For such motors, measure synchronously at a known PWM phase, or average over whole PWM
       periods.
- **Suggested next test (not done yet):** hold one pair on (`ph ab` at 40%) for 30 s and turn the
  rotor by hand.
  - If it detents (snaps to positions), the field works: give it more current or voltage, or a
    slower start.
  - If it turns freely, check the coil directions or the air gap.

---

## 9. Firmware reference (current `six_step/src/main.c`)

Use this as the spec for a rewrite. Structure it into modules: `board`, `drv8316`, `pwm6`, `svpwm`,
`csa`, `limiter`, `cli`, `diag`.

### 9.1 State machine

```
IDLE --start--> ALIGN (step 0, or angle 0 in sine, for align_ms at duty) --> RUN (ramp freq)
RUN/ALIGN --manual--> MANUAL (single steps by key)        MANUAL --auto--> RUN
any --ph x--> PHASE (fixed pattern for phase/pair test)   PHASE --ph exit--> back to autostart
any --stop / Ctrl-C / ESC / nFAULT (2 ms) / overcurrent--> IDLE (+ auto-retry in AUTOSTART builds)
```
- **Start sequence:** clear_fault → csa_calibrate (bridge off) → limiter_reset → load step 0 / sine
  50% → DRVOFF low → MOE = 1 → ALIGN.
- **Stop sequence:** disable TIM6 and the TIM1 update IRQ → MOE = 0 → DRVOFF low → CCER = 0 + COMG →
  duty 0.

### 9.2 Serial commands (USB CDC, any baud, line-based; echo on)

| Command | Function |
|---|---|
| `start`, `stop` | Align + ramp / stop. **Ctrl-C or ESC** = immediate stop from any state. |
| `manual`, `auto` | Freeze on a step; keys `n`/`+`, `b`/`-`, `0`–`5` without Enter; `auto` resumes |
| `duty <0–50>` | Target duty %. In sine mode: line-to-line amplitude % of VIN. |
| `freq <Hz>`, `rpm <n>`, `pp <n>` | Target electrical Hz; mechanical rpm (uses pole pairs); set pole pairs |
| `accel <Hz/s>`, `ramp <rpm/s>` or `ramp off`, `fstart <Hz>` | Ramp = accel + accel_prop·f; `ramp` sets a constant rpm/s; start frequency after align |
| `align <ms>`, `dir` | Alignment time; reverse direction |
| `ilim <A>` | Current limit, 0.2–5 A. Trip at min(2·ilim, 5.3 A). |
| `vf <base%> [slope%/Hz]` | Sine V/f amplitude ceiling |
| `drive sine` / `drive 6step` | Switch drive (restarts the motor in autostart builds). `manual`, `ph` and `watch` are 6-step only. |
| `log`, `watch` | 1 s summary line (+ per-step pin check in 6-step); one line per step |
| `ph a`, `ph ab`, `ph off`, `ph +` or `ph -`, `ph exit` | Phase test: one phase PWM, or first letter PWM + second held low; duty ±5%; exit back to normal |
| `pins` | Gate pin % high, CCRs, MOE/CCER, last ISR SOx raw values |
| `raw <A%> <B%> <C%>` | Static 3-phase duty test (spread ≤ 20%), about 0.5 s, then `scan` and stop |
| `scan` | 2000 free-running samples per SOx: min, max, mean, % of samples above 0.1 A |
| `trace [dec]` | Sine ISR trace: 160 samples (currents in mA, duties in 0.1%, max-duty phase) |
| `diag` | DRV8316 self-test (§9.6) |
| `vin <V>` | Display-only supply voltage for the expected-scope column |
| `status`, `help`, `dfu` | Status; help; reboot into the ROM DFU bootloader |

**Log line (6-step):**
```
[  t.s] RUN  rpm=..  steps/s=.. (expect ..)  duty=..%  freq=.. Hz  I=..A (peak ..A)  nFAULT=high
```
The first line after enabling the log over-counts steps/s: it counts every step since the counter was
last reset. Fix this in the rewrite.

### 9.3 Build-time profiles

| Flag | Effect |
|---|---|
| `AUTOSTART=1` | Start at boot, retry forever, restart after `diag`/`ph exit`/`drive`. Drone profile: duty 40, 200 Hz, accel 5 + 0.1·f, fstart 4 Hz, align 500 ms. |
| `MOTOR=pcb` | PCB profile: pp 2, duty 30, 1000 rpm, +10 rpm/s constant, fstart 1 Hz, align 1000 ms |
| `DRIVE=sine` | Boot in sine mode |

### 9.4 Test tools

- **`flash.py <bin>`:** reboots the running app into DFU, flashes, verifies and waits for the app.
  Rejects files outside 1–128 KB.
- **`phase_test.py`:** interactive keys.
  - `a` `b` → pair A–B (A PWM, B low, C off);
  - a letter then Space → that phase alone;
  - `z` all off, `+`/`-` duty, `s` status, `q` quit (sends `ph exit`).

  Expected at 12 V and 20% duty: about +2.4 V DC across the pair on a DMM, and 0–12 V 20 kHz pulses on
  a scope.
- **`ser.py "cmd" ... "@N"`:** scripted commands. `@N` captures N seconds of log lines. Used for every
  automated test here. Close minicom first; only one program can hold the port.

### 9.5 Scope guide used during bring-up

- **Probe ground on PGND only** (J1 PGND, TP5/TP7). Never on a phase.
- **J2 phase outputs:** 10× probe, 5 V/div, 10 µs/div, edge trigger at about half of VIN.
- **Gate pins** (MCU pins 27–32, 0.5 mm pitch): 1 V/div; dead time at 500 ns/div.
- With no motor, a Hi-Z phase floats and looks noisy. That's normal.

### 9.6 `diag` sequence (all automatic; prints a verdict)

1. Bridge off. Report the 3V3 rail (via VREFINT, `VREFINT_CAL` at 0x1FFF75AA, calibrated at 3.0 V),
   nFAULT logic level + volts (ADC2_IN3), SOA/B/C volts, nSLEEP and DRVOFF.
2. 30 µs nSLEEP pulse; time nFAULT edges for 20 ms using DWT µs timestamps.
3. Sleep 10 ms → wake, watch 300 ms. Sleep 1 s → wake, watch 300 ms.
4. DRVOFF high for 50 ms (expect nFAULT low), then low again (expect high).
5. Verdict:

   | Observation | Verdict |
   |---|---|
   | High and stays high | Driver OK |
   | Cleared by the pulse or sleep | A latched fault, now cleared |
   | Stuck at > 0.15 V, ignores sleep | DRV unpowered: check VM, TP2/TP3, F1, Q1, J1 |
   | Hard 0 V, ignores sleep | Unpowered, or the nFAULT net is shorted |
   | Comes back after wake | Supply-side fault: VM UV, charge pump, AVDD, internal buck |

   Restarts the motor in autostart builds.

**Measured healthy result:**
- nFAULT 3.29 V, unchanged by the reset pulse;
- after wake: low 1.8 ms, then high;
- SOx 1.64 V awake, 0 V asleep;
- 3V3 rail 3.29 V.

---

## 10. Known issues in the current code (fix in the rewrite)

- The header comment says "trip at 4 A" and lists an old command set. The real values are in the
  `#define`s, and `help` prints the current commands.
- The declaration of `raw_hold` has a stray, misplaced comment.
- The first `log` line over-counts steps/s (§9.2).
- **Current sensing is synchronous with SysTick** (1 ms = 20 PWM periods). This is fine for inductive
  motors and wrong for very-low-inductance ones (§8). Sample at a defined PWM phase, ideally ADC
  injected conversions triggered by TIM1 TRGO2 / CC4 at the counter peak, or average over whole PWM
  periods.
- 6-step current uses polled ADC in the SysTick ISR, and `scan` and `raw` use blocking ADC loops. A
  rewrite should use TIM1-triggered injected conversions for all current sampling.
- The sine ISR polls the ADC: about 1.4 µs of busy-wait per period. Use hardware triggering.
- `out()` (printf to USB) blocks in some paths, and long outputs such as `trace` call `tud_task()`
  manually. Keep the motor control entirely in ISRs, as it is now, so USB stalls can't affect it.
- In `MOTOR=pcb` builds, rpm is wrong by the pole-pair error until the magnet count is known.

---

## 11. Hardware fixes: summary table

| Problem | Proposed fix | Status |
|---|---|---|
| Blank chip not visible on USB | BOOT0 (pin 45) → 3V3 jumper + replug → ROM DFU | **Worked.** No longer needed thanks to the `dfu` command. |
| Bootloader dropping or ignoring USB | Tie BOOT0 and USART1_RX (pin 44, PB7) high during DFU entry; stop fwupd | **Worked** together with the next row |
| Random USB disconnects | Reflow the USB-C connector | **Worked** (user resoldered it) |
| Reset button does nothing | Rewire SW4 to GND, or short NRST to GND | Proposed (`README.md` §7.4); workaround: replug |
| USB-C C-to-C power without VIN | CC resistors as Rd (5.1 kΩ to GND) | Proposed (`README.md` §5.3) |
| nFAULT stuck low at idle | **Firmware:** DRVOFF low at idle | **Worked** |
| DRV "Mode 2" suspicion (R8) | R8 = 0 Ω to GND | User reports R8 is shorted; SOx working agrees. Confirm with a DMM. |
| No current when all three phases PWM (blocks sine/SVPWM) | **Tie OCP/SR pin 35 to GND** (ASR/AAR off), then power-cycle | **Proposed, not yet done.** Re-run the §6.3 `raw` table afterwards. |
| Current range too small for a > 5 A limit | Tie GAIN pin 36 to GND (0.15 V/A, about ±10.9 A). **Change `CSA_GAIN_V_PER_A` in firmware.** | Optional; not done. The DRV is still limited to 8 A peak. |
| PSU stops near 0.5 A | Raise the PSU current limit to 1–1.5 A (keep 9 V) | Suspected; user to check the CC indicator |
| PCB motor doesn't move | Hand-detent test (§8); check coil directions and air gap; more current/voltage | **OPEN** |

---

## 12. Recommended order for the rewrite

1. **Board layer.** Clocks: 170 MHz PLL + HSI48/CRS. GPIO/AF table. DWT. A USB CDC console that keeps
   the `dfu` command. Prove it with `flash.py`.
2. **DRV8316 layer.** Idle = DRVOFF low + INx low; `clear_fault`; nFAULT debounce; `diag`. Prove it:
   `diag` reports "driver OK".
3. **PWM layer.** TIM1 centre-aligned 20 kHz, 1 µs dead time, CCPC/COMG. A gate read-back self-test.
   Prove it with the §4.2 table and the phase test.
4. **CSA layer.**
   - TIM1-triggered injected conversions of SOA/SOB/SOC at the counter peak.
   - Offset calibration with the bridge off.
   - **Gain from a single constant.**
   - Prove it: `raw 10 0 0` gives about +2.9 A on A and −1.6 A on B and C (drone motor, 9 V).
5. **6-step** with the limiter, trip and auto-retry. Prove it with the §4.4 table.
6. **After pin 35 is grounded,** re-run the §6.3 table. Then SVPWM open-loop (V/f).
7. **Then sensorless FOC.** The CSA works, and `README.md` §12 describes the intended control loop.
