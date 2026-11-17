# Common board layer

Shared by every application. Nothing here starts the motor or enables the
bridge; that is always explicit in the app.

## Files

| File | Role |
|---|---|
| `board.c` / `board.h` | Clock, safe GPIO, peripheral init, PWM, DPWMMIN helper, DRV helpers, USART1 link, newlib syscalls, `HAL_Delay` override |
| `usb_console.c` / `.h` | USB CDC console (TinyUSB, HSI48 + CRS), `dfu` reboot command, Ctrl-C/ESC stop |
| `usb_descriptors.c`, `tusb_config.h` | TinyUSB descriptors (VID `cafe:4001`, product `STM32G431 <app>`) and config |
| `debug.h` | Direct stdio redirection (printf/fputs/putchar) to the console |
| `stm32g4xx_hal_msp.c` | Peripheral clocks and pin alternate functions |
| `stm32g4xx_it.c` | Exception and peripheral interrupt handlers (DMA1, I2C1, USART1) |
| `stm32g4xx_hal_conf.h` | Lean HAL module configuration |
| `system_stm32g4xx.c` | CMSIS system init and FPU enable |
| `startup_stm32g431cbtx.s` | Vector table and reset handler |
| `sysmem.c` | Newlib heap allocator (_sbrk) |
| `STM32G431CBTX_FLASH.ld` | Linker script, flash limited to 126 KB, `.noinit` RAM for the DFU request word |

## Boot state

`Board_Init()` performs:

1. Jump to the ST ROM bootloader if the last run asked for it (`dfu`).
2. `HAL_Init()` and the board MSP init (including UCPD dead-battery disable).
3. 170 MHz clock from HSI16 through the PLL (M=4, N=85, R=2), boost mode.
   There is no crystal on the board.
4. DWT cycle counter (for `Board_DelayUs()`).
5. Safe GPIO:
   - nSLEEP (PC13) low, driver asleep
   - DRVOFF (PA7) **low**. High would pull nFAULT low (DRV8316 §8.4.2).
   - INHx/INLx (PA8/9/10, PB13/14/15) driven low with pull-downs, so the
     bridge is Hi-Z
   - nFAULT (PA6) input with pull-up
6. USART1 TX only on PB6 at 115200 8N1 (console fallback).
7. USB CDC console: HSI48 trimmed by the CRS to USB start-of-frame.

PB7 is left for the encoder SDA. USART1 RX is not enabled initially.

## Peripheral init

Call only what the application needs, in this order where relevant:

```c
Board_Init();
Board_InitI2C1();       /* PA15/PB7, 400 kHz */
Board_InitTim1(arr);    /* centre-aligned PWM, 3 complementary pairs */
Board_Tim1AdcTrigger(); /* CH4/OC4REF -> TIM1_TRGO at the counter peak */
Board_InitAdc1();       /* PA0/PA1/PA2, TIM1_TRGO, circular DMA */
```

For applications interfacing with the ESP32:
```c
Board_InitUsart1Link(230400); /* reconfigures PB6 to half-duplex open-drain */
```

TIM1 dead time is 1.0 µs (`PWM_DEADTIME = 0x80 | 21`). In 6x PWM mode the
DRV8316 adds none, and the SLEW strap (125 V/µs) needs about 1 µs.

`Board_Tim1StartPwm()` sets all channels to 50 percent and starts them,
including the complementary outputs. `Board_Tim1StopPwm()` disables MOE and
stops all six. `Board_BridgeOff()` is the safe stop from any context:
MOE = 0 and DRVOFF low.

`Board_DutyDpwmMin(va, vb, vc, vbus, d)` turns phase voltages into
bottom-clamped duties: the lowest phase is exactly 0 %. Use it for any
voltage vector. With DRV8316 pin 35 floating, plain sine or SVPWM (all three
phases switching) passes almost no current.
`Board_SetPwmPhases(da, db, dc)` writes duties in A, B, C order.

`Board_SetPwmRaw(ch1, ch2, ch3)` takes duties in the board order:

```
ch1 -> INHC -> phase C
ch2 -> INHB -> phase B
ch3 -> INHA -> phase A
```

## Driver helpers

| Function | Effect |
|---|---|
| `Board_DrvSleep()` | nSLEEP low |
| `Board_DrvWake()` | nSLEEP high |
| `Board_DrvOutputEnable()` | DRVOFF low (the normal state) |
| `Board_DrvOutputDisable()` | DRVOFF high: emergency kill only; nFAULT reads low while it is held |
| `Board_DrvFault()` | 1 if nFAULT is low |
| `Board_DrvFaultDebounced()` | 1 if nFAULT is still low after 2 ms |
| `Board_DrvClearFault()` | 30 µs nSLEEP pulse (tRST 20–40 µs), then 2 ms |
| `Board_DrvInit()` | Wait 5 ms after wake (nFAULT is low ~1.8 ms), clear if needed |

After any sleep/wake cycle, call `DRV8316_Init()`. The DRV8316T is strapped
for hardware mode (MODE=6x PWM, GAIN=0.3 V/A, SLEW=125 V/us, OCP/SR open).

## Console & Link

`DBG_Puts()` and `DBG_Printf()` go to the USB CDC port while a host has it
open, and to USART1 TX on PB6 otherwise. Output from interrupt context is
never sent to USB. `HAL_Delay()` and `Board_DelayMs()` service USB while they
wait. In long busy loops, call `Board_Poll()`. When `Board_InitUsart1Link()`
is called, PB6 becomes half-duplex open-drain for the framed ESP32 protocol
at 230400 baud.

USB console commands, in every app: `dfu` reboots into the ROM bootloader,
which `make flash` uses. `Ctrl-C` or `ESC` calls `Board_BridgeOff()` and sets
`Board_StopRequested()`.

## Interrupts

`stm32g4xx_it.c` implements SysTick, DMA1 Channel 1, I2C1 event/error, and USART1.
`usb_console.c` implements the three USB interrupts. The ADC DMA interrupt is
priority 1, I2C1 and USART1 are priority 5, and USB is priority 6, so the
control loop cannot be preempted by the encoder, link or console.
