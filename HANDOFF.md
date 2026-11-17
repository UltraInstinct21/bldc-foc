# Handoff: board v7 bring-up, drive lab, DRV8316 debugging

Written 2026-11-18 at the end of a Claude Code session on the original laptop,
so the work can continue on another machine. It records the hardware state,
what was changed and verified, what is broken right now, and exactly what to
do next.

| Item | Value |
|---|---|
| Repository | <https://github.com/UltraInstinct21/bldc-foc>, branch `main` |
| Last commits from the session | `bf1cb5a` (merge of `board-v7-bringup` into `main`), then the commit that added this file |
| Claude Code session ID | `015c0b42-d4f5-45d3-ad99-d9f4b6129f17` |
| Session transcript on the old laptop | `~/.claude/projects/-home-sarthi-Projects-bldc-foc/015c0b42-d4f5-45d3-ad99-d9f4b6129f17.jsonl` |
| Board currently flashed with | `test_09_drv_diag` (bridge never enabled; safe to leave powered) |

---

## 1. Continue with Claude Code on the new laptop

### Option A: start fresh from this file (recommended)

```sh
git clone https://github.com/UltraInstinct21/bldc-foc.git ~/Projects/bldc-foc
cd ~/Projects/bldc-foc
claude
```

First prompt:

> Read HANDOFF.md, docs/hardware.md, docs/control.md and
> docs/results/2026-11-18_drive_lab.md. Then continue from "Next steps" in
> HANDOFF.md, starting with the DRV8316 fault.

Everything the session learned is in the repo; nothing depends on the old
transcript.

### Option B: carry the transcript over

Copy the `.jsonl` file above to the new laptop, into
`~/.claude/projects/<mangled project path>/`. The folder name is the absolute
project path with `/` replaced by `-` (for `/home/sarthi/Projects/bldc-foc` it is
`-home-sarthi-Projects-bldc-foc`). Clone the repo to the **same absolute path**,
then run `claude --resume` in it and pick the session. If the username or path
differs, use option A: the old transcript also refers to `/tmp/claude-1000/...`
scratchpad files that will not exist on the new machine.

---

## 2. Set up the new laptop

Tested on Ubuntu with the Ubuntu packages:

```sh
sudo apt install gcc-arm-none-eabi libnewlib-arm-none-eabi dfu-util openocd python3 usbutils
sudo usermod -aG dialout,plugdev $USER          # log out and in afterwards
```

udev rules (recommended):

```sh
# ST ROM DFU bootloader without sudo
echo 'SUBSYSTEM=="usb", ATTR{idVendor}=="0483", ATTR{idProduct}=="df11", MODE="0666"' | \
  sudo tee /etc/udev/rules.d/50-stm32-dfu.rules
# Keep ModemManager away from the board's USB serial port (cafe:4001)
echo 'ATTRS{idVendor}=="cafe", ATTRS{idProduct}=="4001", ENV{ID_MM_DEVICE_IGNORE}="1"' | \
  sudo tee /etc/udev/rules.d/51-bldc-foc-no-modemmanager.rules
sudo udevadm control --reload-rules && sudo udevadm trigger
```

ModemManager probed the board's `/dev/ttyACM0` on every enumeration during the
session (`ID_MM_CANDIDATE=1`), toggling DTR and stealing console output. If the
DFU device disappears during a flash, also run `sudo systemctl stop fwupd`.

Check the toolchain:

```sh
cd ~/Projects/bldc-foc/firmware
make apps                     # 12 apps, expect no warnings
make apps ENCODER_MODE=adc    # also builds clean
```

---

## 3. Hardware: the board as it is right now

Board v7 (a file rename of v6; KiCad files still named `bldc_control_final_v6`).
Full reference: `varun_foc_context_README.md` and [`docs/hardware.md`](docs/hardware.md).

| Item | State |
|---|---|
| MCU | STM32G431CBT6, no crystal (HSI16 → PLL 170 MHz, HSI48 + CRS for USB) |
| Driver | DRV8316**T** (hardware straps; BOM says DRV8316RRGFR) |
| DRV8316 pin 35 OCP/SR | **Reworked to PGND by the user** (was floating = ASR/AAR on) |
| DRV8316 pin 36 GAIN | **Reworked to PGND by the user** → CSA gain is now **0.15 V/A**, not 0.3 V/A. Firmware is NOT updated yet |
| J7 | USART1 TX (PB6), USART1 RX (PB7) and BOOT0 (PB8, pin 45) **shorted together**, not connected to anything. The user ties this bundle to 3V3 only to force the ROM bootloader (there is no boot switch). Firmware default `BOARD_J7_BOOT_STRAP=1` never drives PB6 |
| Motor on J2 | 12N14P drone outrunner, 7 pole pairs; measured R = 0.138 Ω/phase, L ≈ 11 µH, λ ≈ 0.0009 Wb |
| Supply | 9 V on J1 (6 V caused DRV8316 VM undervoltage trips; use ≥ 9 V) |
| AS5600 | Not answering at 0x36 (probably not connected) |
| ESP32 | Not connected (and the link can't work while J7 is shorted) |
| USB-C | Works; C-to-A cable preferred (CC resistors are pull-ups). The connector joints were reflowed once during bring-up |
| Reset button SW4 | Does nothing (wired to 3V3). Replug to reset |

### ⚠ Open problem: the DRV8316 is faulted after the pin 35/36 rework

`test_09_drv_diag` output, last run:

```
3V3 rail (VDDA) = 3.293 V
asleep (nSLEEP low):   nFAULT high (3.29 V) | SOA 0.001 SOB 0.184 SOC 0.347 V
awake 5 ms after wake: nFAULT LOW  (0.02 V) | SOA 1.655 SOB 0.272 SOC 0.312 V
nFAULT low for 300000 us after wake (healthy: ~1700-1800 us)
30 us nSLEEP pulse: nFAULT still LOW
DRVOFF high: nFAULT LOW | DRVOFF low: nFAULT LOW (expect high)
VERDICT: nFAULT net OK (high while asleep), but the DRV asserts a fault on every wake
         that the nSLEEP pulse does not clear: a persistent supply-side fault.
         SOB = 0.272 V awake (expect ~1.65 V): output loaded or shorted
         SOC = 0.329 V awake (expect ~1.65 V): output loaded or shorted
```

Before the rework, the same board passed everything (nFAULT low 1678 µs after
wake, SOx 1.650/1.652/1.650 V). Interpretation:

- **nFAULT net is fine** (rises to 3.29 V in sleep). Not an nFAULT short.
- **The chip is powered and reacts to nSLEEP**, but asserts a fault on every
  wake that the 30 µs reset pulse does not clear. Candidates from the datasheet
  fault table: **charge-pump UV** (pins 6–8, C26, C27), **buck UV** (FB_BK pin 3
  < 2.6 V; R5, C29), **AVDD UV** (pin 25, C24), VM UV (pins 9–11).
- **SOA is healthy (1.655 V = VREF/2), so VREF (pin 37) is OK.** In the run
  before this one all three SOx were ~0.2 V; something changed in between.
- **SOB (pin 39) and SOC (pin 38) read 0.18–0.35 V even while asleep** (should
  be ~0 V asleep, ~1.65 V awake). Something external feeds them. Prime suspect:
  a resistive solder/flux bridge **37 (3V3) ↔ 38 ↔ 39**, next to the reworked
  pins 35/36.

Measurements still to take (VIN 9 V on, black lead on PGND TP5/TP7;
`test_09_drv_diag` keeps the DRV awake ~5 s of every ~6 s):

| Point | Expected | If wrong |
|---|---|---|
| C26 / pin 8 (CP) | ≈ VM + 5 V (~14 V) | charge-pump UV |
| C29 / pin 3 (FB_BK) | ~3.3 V | buck UV if < 2.6 V |
| C24 / pin 25 (AVDD) | ~3.3 V | AVDD UV → chip in reset |
| TP3 / pins 9–11 (VM) | ~9 V | supply path F1/Q1 |
| Pin 37 (VREF) | 3.3 V | VREF |
| Pins 38, 39 (SOC, SOB) | ~1.65 V | bridge / damage |

Power off, continuity: pins 35, 36 → GND = 0 Ω (intended); pin 37 → TP1 (3V3)
= 0 Ω; pin 37 → GND not 0 Ω; 34↔35, 36↔37, 37↔38, 38↔39 must be **open**;
pins 25 and 3 → GND not 0 Ω. Inspect 33–40 under magnification, clean with
IPA, wick bridges.

---

## 4. What changed in the firmware (all in `main`)

Source of truth for the board: `varun_foc_context_README.md` (hardware) and
`varun_foc_context/varun_foc/FIRMWARE_README.md` (bring-up log). The repo was
brought in line with them:

| Area | Change | Why |
|---|---|---|
| Idle state | DRVOFF **low** + all six INx low (MOE = 0), gate pins pulled down. `Board_BridgeOff()` everywhere | DRVOFF high makes the DRV8316 pull nFAULT low |
| Dead time | 1.0 µs (`PWM_DEADTIME = 0x80 \| 21`), was 753 ns | 6x PWM mode, SLEW 125 V/µs, driver adds none |
| Fault clear | 30 µs nSLEEP pulse (DWT timed), 5 ms after wake, 2 ms nFAULT debounce | tRST 20–40 µs; nFAULT low ~1.8 ms after wake |
| CSA | Sign **+1** (measured), offsets checked 1.3–2.0 V, DRV woken before offsets | SOx rises for current out of the phase |
| Modulation | FOC and calibration use **bottom-clamped DPWM**; `FOC_USE_SVPWM=1` for after pin 35 is grounded | With pin 35 floating, all-phase switching passed no current |
| Encoder calibration | Modulation 0.025 (was 0.01), ceiling 0.03; test_06 angle convention fixed | 1 µs dead time swallowed 0.01; test_06 was 90° off FOC |
| `FOC_Enable()` | Sets MOE explicitly | foc_esp could never re-enable after a disable |
| USB-C | TinyUSB 0.17.0 CDC console (`firmware/Middlewares/tinyusb`), HSI48 + CRS, `dfu` command, Ctrl-C/ESC = bridge off, waits ≤ 1 s + 3 s at boot for a host | User wanted USB-C as default for flashing and console |
| Boot from DFU | `-DUSER_VECT_TAB_ADDRESS`, `HAL_RCC_DeInit()` first, USB IRQs held off and USB peripheral reset | `dfu-util :leave` jumps into the app without a reset |
| J7 strap | `BOARD_J7_BOOT_STRAP=1` (default): no UART console, ESP32 link refuses to start | PB6/PB7/BOOT0 are shorted on this board |
| Flashing | `make flash` → `tools/flash.py`: USB DFU if the board is on USB, else ST-Link. `flash-dfu`, `flash-swd` force one | |
| `USER_DEFS` | `make APP=x USER_DEFS="-DFOO=1"` | Overrides without editing code |

New apps and tools:

| Path | Purpose |
|---|---|
| `firmware/tests/test_05_open_loop` | **Verified** open-loop 6-step with current limiter, trip, auto-retry (see [`docs/trapezoidal.md`](docs/trapezoidal.md)) |
| `firmware/tests/test_08_drive_lab` | Interactive lab over USB console: `mode 6step\|sine\|svpwm\|dpwm`, `raw`, `rl`, `if`, `sl`, `trace`, `set ...`, `help` |
| `firmware/tests/test_09_drv_diag` | Analog DRV8316 diagnostics (3V3, nFAULT volts, SOx volts, wake/pulse/DRVOFF), repeats every 5 s |
| `firmware/tools/flash.py` | Flasher (USB DFU first, ST-Link fallback) |
| `firmware/tools/run_test.sh` | Flash an app over DFU and capture its console from the first line |
| `firmware/tools/read_port.py` | Read the USB console with DTR held (`STOP_AT_END=1` sends ESC at the end) |
| `firmware/tools/lab.py` | Scripted `test_08` sequences with logging |
| `firmware/tools/observer_replay.py` | Offline replay of the flux observer on a recorded trace |

---

## 5. Test status (before the pin 35/36 rework)

| Test | Result |
|---|---|
| `test_00_hello` | ✅ USB enumeration, DFU flashing (both directions), DRVOFF=0 idle |
| `test_02_drv8316` | ✅ nFAULT low 1678 µs after wake, DRVOFF check PASS |
| `test_01_as5600` | ⚠ no answer at 0x36 (encoder likely not connected) |
| `test_03_pwm` | ✅ DPWM pattern (scope still needed for 20 kHz / 1 µs) |
| `test_04_current_sense` | ✅ offsets 1.650 / 1.652 / 1.650 V, ±10 mA noise |
| `test_05_open_loop` | ✅ 9 V: 4 → 200 Hz el. (1714 rpm), duty 24 % at 200 Hz, held 8 s. ❌ 6 V: VM UVLO trips |
| `test_06_encoder_cal`, `foc`, `foc_esp` | not run (need AS5600) |
| `test_07_esp_link` | not run (J7 short) |
| `test_08_drive_lab` | see below and [`docs/results/2026-11-18_drive_lab.md`](docs/results/2026-11-18_drive_lab.md) |
| `test_09_drv_diag` | ❌ driver faulted after rework (section 3) |

Drive lab (pin 35 floating, 9 V):

| Mode / function | Result |
|---|---|
| 6-step | runs to 180 Hz in 15 s |
| Sine, SVPWM | 0.01 A, interlock trip (expected with pin 35 floating) |
| DPWM | runs to 180 Hz, ~50 % sector dropouts above 50 Hz |
| `rl` | R 0.138 Ω, L 10–12.6 µH |
| `if` (dq current loop on SOx, open-loop angle) | id 1.5 ± 0.1 A, 0–43 Hz; dropouts and lost sync ~50 Hz |
| `sl` (sensorless) | observer tracked 30.9 / 34.8 Hz at 30.8 / 35.0 Hz; ran closed loop 35 → 44 Hz, then lost lock |

---

## 6. Key findings (do not rediscover these)

1. **DRVOFF high pulls nFAULT low.** Never idle with DRVOFF high.
2. **6x PWM mode: the MCU owns the dead time** (1 µs).
3. **CSA sign is +1**, offset ~1.65 V, gain 0.3 V/A with GAIN open (**0.15 V/A
   now that pin 36 is grounded**), ADC saturates ±5.4 A at 0.3 V/A.
4. **Pin 35 floating (ASR/AAR) blocks current when all three phases switch**,
   floats a phase near its current zero crossing, stops conduction at low
   current, and **removes the dead-time voltage loss** (observer must use the
   written duties with no dead-time subtraction; dead-time compensation still
   helps conduction). All of this should change once pin 35 is grounded:
   re-check with `raw` and `observer_replay.py`.
5. **Current-loop safety:** bound the voltage by the motor model
   (`2·R·ilim + 1.5·ω·(λ + L·ilim) + 0.3 V`) and hold the integrators during
   dropouts, or a dropout winds the PI up and the returning current trips OCP.
6. **VM UVLO is 4.1–4.3 V with a 3–7 µs deglitch.** 6 V supply trips it under
   load. Use ≥ 9 V.
7. **After `dfu-util :leave` the app starts without a reset**: VTOR and RCC
   must be reinitialised (done).
8. **The USB console only prints while DTR is set.** Use `tools/read_port.py`
   or a terminal (picocom/minicom) that keeps the port open; avoid `stty`+`cat`.
9. **The AS5600 SDA pull-up also pulls BOOT0 high** (J7 short), so a cold boot
   with the encoder attached lands in the ROM bootloader. `make flash` handles
   it; to just start the app, flash once more.

---

## 7. Everyday commands

```sh
cd ~/Projects/bldc-foc/firmware

make APP=test_09_drv_diag && tools/run_test.sh test_09_drv_diag 5      # diagnostics
make APP=test_05_open_loop && STOP_AT_END=1 tools/run_test.sh test_05_open_loop 25
make APP=test_08_drive_lab flash                                       # lab
python3 tools/lab.py --log ../docs/results/run.log raw 6step sine svpwm dpwm rl if
python3 tools/lab.py --log ../docs/results/run.log "cmd:set lam 0.0009" sl
picocom /dev/serial/by-id/usb-Custom_STM32G431_*-if00                   # manual console
```

The lab keeps R, L and λ only in RAM. After a reflash, either run `rl` or
`cmd:set R 0.1378` and `cmd:set L 11.3` (µH), plus `cmd:set lam 0.0009` for `sl`.

Recovery:

| Situation | Fix |
|---|---|
| Board not on USB, D1 lit | Jumper BOOT0 bundle (J7) to 3V3, replug, `make APP=... flash-dfu`, remove jumper |
| Board not on USB, D1 off | No power: C-to-A cable or VIN on J1 |
| `nFAULT low and would not clear` | `test_09_drv_diag`; VIN; see section 3 |
| Console silent | Port opened without DTR or by ModemManager; use `read_port.py` |

---

## 8. Next steps, in order

1. **Fix the DRV8316 fault** (section 3). Done when `test_09_drv_diag` prints
   `VERDICT: driver OK` with SOA/SOB/SOC ≈ 1.65 V.
2. **Update the CSA gain for GAIN = GND:** set `DRV8316_CSA_V_PER_A` to
   `0.15f` in `firmware/common/board.h` (single constant; test_04/05/08, foc
   and foc_esp all use it). Range becomes about ±10.9 A; the DRV is still 8 A
   peak, so keep limits as they are. Re-check offsets with `test_04`.
3. **Re-characterise with pin 35 grounded** (ASR/AAR off):
   - `test_08`: `raw` table. 56/44/50 should now pass current like 20/0/10.
   - `sine` and `svpwm` open loop (the user's original request:
     "flash the svpwm code and monitor").
   - `rl`, then `if` with `set focmod svpwm`, then `sl`.
   - Record a new 35 Hz I/f trace and run `observer_replay.py`: the dead-time
     loss will probably be back; compare the models it prints, then set the
     firmware `vrec` and `dtc` options to match.
   - Build `foc` with `USER_DEFS=-DFOC_USE_SVPWM=1` once SVPWM is confirmed.
   - Update `docs/control.md`, `docs/hardware.md`, `docs/trapezoidal.md` §2
     and write `docs/results/<date>_pin35_grounded.md`.
4. **AS5600**: wire it (SCL J6.2, SDA J7.2, 3V3/GND from J6, 4.7 k pull-ups;
   J7 bundle must NOT be tied to 3V3), then `test_01`, `test_06`, `foc`.
5. **J7**: remove the TX/RX/BOOT0 short to use the ESP32; build with
   `USER_DEFS=-DBOARD_J7_BOOT_STRAP=0`; then `test_07`, `foc_esp`.
6. Cosmetic: the `[console]` greeting sometimes prints twice when the port is
   opened.

---

## 9. Where to read more

| File | Contents |
|---|---|
| [`varun_foc_context_README.md`](varun_foc_context_README.md) | Hardware reference for board v7 (source of truth) |
| [`varun_foc_context/varun_foc/FIRMWARE_README.md`](varun_foc_context/varun_foc/FIRMWARE_README.md) | 2026-11-17 bring-up log |
| [`docs/hardware.md`](docs/hardware.md) | Straps, driver behaviour, caveats 1–14 |
| [`docs/control.md`](docs/control.md) | FOC, modulation analysis, sampling window |
| [`docs/trapezoidal.md`](docs/trapezoidal.md) | Six-step commutation in detail |
| [`docs/bringup.md`](docs/bringup.md) | Flashing, test order, calibration |
| [`docs/results/2026-11-18_drive_lab.md`](docs/results/2026-11-18_drive_lab.md) | All drive-lab measurements (+ `.log`, `_if35_trace.txt`, `_if_fault_trace.txt`) |
| [`firmware/README.md`](firmware/README.md), [`firmware/tests/README.md`](firmware/tests/README.md) | Build, flash, per-test expected output |
