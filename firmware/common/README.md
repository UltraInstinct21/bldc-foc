# Common board layer

Shared by every application. Nothing here starts the motor or enables the
bridge; that is always explicit in the app.

## Files

| File | Role |
|---|---|
| `board.c` / `board.h` | Clock, safe GPIO, peripheral init, PWM, DRV helpers, USART1 link |
| `debug.c` / `debug.h` | Blocking UART console on USART1 TX |
| `main.h` | Shared prototypes |
| `stm32g4xx_hal_msp.c` | Peripheral clocks and pin alternate functions |
| `stm32g4xx_it.c` | Exception and peripheral interrupt handlers (DMA1, I2C1, USART1) |
| `stm32g4xx_hal_conf.h` | HAL module selection |
| `system_stm32g4xx.c` | CMSIS system init and FPU enable |
| `startup_stm32g431cbtx.s` | Vector table and reset handler |
| `syscalls.c`, `sysmem.c` | Newlib stubs |
| `STM32G431CBTX_FLASH.ld` | Linker script, flash limited to 126 KB |

## Boot state

`Board_Init()` performs:

1. `HAL_Init()` and the board MSP init (including UCPD dead-battery disable).
2. 170 MHz clock from HSI16 through the PLL (M=4, N=85, R=2), boost mode.
3. Safe GPIO:
   - nSLEEP (PC13) low, driver asleep
   - DRVOFF (PA7) high, outputs Hi-Z
   - nFAULT (PA6) input with pull-up
4. USART1 TX only on PB6 at 115200 8N1.

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

`Board_Tim1StartPwm()` sets all channels to 50 percent and starts them,
including the complementary outputs. `Board_Tim1StopPwm()` disables MOE and
stops all six.

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
| `Board_DrvOutputEnable()` | DRVOFF low, power stage active |
| `Board_DrvOutputDisable()` | DRVOFF high, power stage Hi-Z |
| `Board_DrvFault()` | 1 if nFAULT is low |

After any sleep/wake cycle, call `DRV8316_Init()`. The DRV8316CT is strapped
for hardware mode (MODE=6x PWM, GAIN=0.3 V/A, SLEW=125 V/us).

## Console & Link

`DBG_Puts()` and `DBG_Printf()` are blocking and TX only on PB6. When
`Board_InitUsart1Link()` is called, PB6 becomes half-duplex open-drain for
the framed ESP32 protocol at 230400 baud.

## Interrupts

`stm32g4xx_it.c` implements SysTick, DMA1 Channel 1, I2C1 event/error, and USART1.
The ADC DMA interrupt is priority 1, I2C1 is priority 5, and USART1 is priority 5,
so the control loop cannot be preempted by the encoder or link transfers.
