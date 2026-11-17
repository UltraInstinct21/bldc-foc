/**
 * @file    board.h
 * @brief   Pin map and peripheral bring-up for bldc_control_final_v6 (board v7).
 *
 * Board: STM32G431CBT6 (LQFP-48) + DRV8316T (hardware-strap variant).
 * v7 is a file rename of v6: same schematic and layout.
 *
 * Every test app and the final FOC app call Board_Init() first, then only
 * the peripheral init functions they need. Nothing here starts the bridge;
 * that is always explicit.
 *
 * Accessible pins on this board revision (from the KiCad PCB):
 *
 *   J3: USB-C, PA11 = D-, PA12 = D+  (console + DFU flashing, the default)
 *   J5: 1 = 3V3, 2 = SWDIO, 3 = SWCLK, 4 = GND  (no reset wire)
 *   J6: 1 = 3V3, 2 = PA15, 3 = GND
 *   J7: 1 = PB6 (USART1_TX), 2 = PB7 (USART1_RX / I2C1_SDA), no GND
 *   J2: motor phases C / B / A
 *
 * The AS5600 is wired to the J6/J7 pins:
 *   SCL = PA15 (J6.2), SDA = PB7 (J7.2), VCC = J6.1, GND = J6.3.
 *
 * DRV8316T straps as built (verified during bring-up, 2026-11-17):
 *   MODE    pin 33  R8 0R to GND      6x PWM, no internal dead time
 *   SLEW    pin 34  R7 47k to AVDD    125 V/us -> 1 us dead time in TIM1
 *   OCP/SR  pin 35  open (floating)   ASR + AAR active demagnetisation ON
 *   GAIN    pin 36  open              0.3 V/A
 *   VREF    pin 37  3V3               CSA bias VREF/2 = 1.65 V
 *
 * Two rules come out of the bring-up and are enforced here:
 *
 *  1. Idle = DRVOFF LOW + all six INx low (MOE = 0). DRVOFF high makes the
 *     DRV8316 pull nFAULT low (datasheet 8.4.2), so it is NOT an idle state;
 *     it is only an emergency kill (Board_DrvOutputDisable()).
 *
 *  2. With OCP/SR floating (ASR/AAR on), the bridge passes almost no current
 *     when all three phases switch. Current flows when at least one phase's
 *     low side is held on with no PWM edges. So never drive a voltage vector
 *     with plain sine/SVPWM: use Board_DutyDpwmMin() (lowest phase at 0 %),
 *     or 6-step. Tying pin 35 to GND would lift this; it has not been done.
 */
#ifndef BOARD_H
#define BOARD_H

#include <stdint.h>
#include "stm32g4xx_hal.h"

/* ---- DRV8316 control ---------------------------------------------------- */
#define DRV_NSLEEP_PORT     GPIOC
#define DRV_NSLEEP_PIN      GPIO_PIN_13
#define DRV_DRVOFF_PORT     GPIOA
#define DRV_DRVOFF_PIN      GPIO_PIN_7
#define DRV_NFAULT_PORT     GPIOA
#define DRV_NFAULT_PIN      GPIO_PIN_6

/* Current-sense amplifiers. SOx = VREF/2 (1.65 V) at 0 A, rising with current
 * flowing out of the phase into the motor (measured).
 * GAIN pin 36: open = 0.3 V/A (bring-up, ADC saturates ~+/-5.4 A);
 *              GND  = 0.15 V/A (reworked 2026-11-18, ~+/-10.9 A range).
 * If the real gain were still 0.3 V/A, this setting makes every current read
 * 2x high: limiters then act early, i.e. the safe direction. */
#ifndef DRV8316_CSA_V_PER_A
#define DRV8316_CSA_V_PER_A     0.15f
#endif
#define DRV8316_CSA_OFF_MIN_V   1.3f    /* zero-current offset sanity window */
#define DRV8316_CSA_OFF_MAX_V   2.0f

/* Backward compatibility aliases */
#define DRV8316_Init        Board_DrvInit
#define DRV8316_ClearFault  Board_DrvClearFault
#define DRV8316_Fault       Board_DrvFault

/* ---- J7 boot strap -------------------------------------------------------
 * On the current board BOOT0 (PB8, pin 45), USART1_TX (PB6, J7.1) and
 * USART1_RX (PB7, J7.2) are wired together: there is no BOOT0 switch, and the
 * bundle is tied to 3V3 only to enter the ROM bootloader (BOOT0 high, plus
 * USART1_RX high so the bootloader does not lock onto a floating UART).
 * With BOARD_J7_BOOT_STRAP = 1 (default):
 *   - PB6 is never driven: no UART console (USB only) and no ESP32 link
 *     (Board_InitUsart1Link() stops with a message). Driving PB6 would fight
 *     the 3V3 tie, and the link would collide with the AS5600 SDA on PB7.
 *   - I2C1 on PA15/PB7 still works while the bundle is NOT tied to 3V3. An
 *     AS5600 pull-up on SDA then also pulls BOOT0 high, so a cold power-up
 *     starts the ROM bootloader (0483:df11); "make flash" handles that.
 *   - Never run an I2C app with the bundle tied to 3V3: SDA would be driven
 *     low against the rail.
 * Build with USER_DEFS=-DBOARD_J7_BOOT_STRAP=0 once the short is removed. */
#ifndef BOARD_J7_BOOT_STRAP
#define BOARD_J7_BOOT_STRAP     1
#endif

/* ---- console and ESP32 link --------------------------------------------
 * printf goes to the USB CDC port when a host has it open, otherwise to
 * USART1 TX on PB6 (J7.1). Board_InitUsart1Link() switches PB6 to
 * half-duplex open-drain for the ESP32; PB7 stays free for the encoder SDA. */
#define DBG_UART_BAUD       115200u
#define LINK_UART_BAUD      230400u

/* ---- AS5600 encoder ----------------------------------------------------- */
#define ENC_SCL_PORT        GPIOA
#define ENC_SCL_PIN         GPIO_PIN_15     /* J6.2 */
#define ENC_SDA_PORT        GPIOB
#define ENC_SDA_PIN         GPIO_PIN_7      /* J7.2, shares USART1_RX */

/* ---- motor PWM: TIM1 ---------------------------------------------------- */
#define PWM_TIM             TIM1
#define PWM_ARR_20KHZ       4249u           /* 170 MHz / (2*(4249+1)) = 20 kHz */
/* 1.0 us dead time, required by SLEW = 125 V/us in 6x PWM mode (the DRV adds
 * none). DTG = 0x80 | 21 -> (64 + 21) * 2 = 170 ticks at 170 MHz. */
#define PWM_DEADTIME        (0x80u | 21u)

/* ---- current sense: ADC1 ------------------------------------------------ */
#define ADC_BUF_LEN         3u

/* ---- shared HAL handles ------------------------------------------------- */
extern UART_HandleTypeDef huart1;
extern I2C_HandleTypeDef  hi2c1;
extern TIM_HandleTypeDef  htim1;
extern ADC_HandleTypeDef  hadc1;
extern DMA_HandleTypeDef  hdma_adc1;

/* ---- init --------------------------------------------------------------- */
/*
 * DFU check, clock, safe GPIO, DWT, UART console, USB console. Leaves:
 *   nSLEEP low (DRV asleep), DRVOFF low, the six gate pins driven low.
 */
void Board_Init(void);

/* Switch USART1 to half-duplex open-drain on PB6 for the ESP32 link.
 * Call once at startup, after Board_Init(). Requires a 4.7k pull-up on
 * the line; the UART console fallback keeps working on the same pin. */
void Board_InitUsart1Link(uint32_t baud);

void Board_InitI2C1(void);              /* PA15/PB7, 400 kHz */
void Board_InitTim1(uint32_t arr);      /* centre-aligned PWM, 3 + 3 outputs */
void Board_InitAdc1(void);              /* PA0/PA1/PA2, TIM1_TRGO, DMA */

/* Route TIM1_CH4/OC4REF to the ADC trigger at the counter peak, where all
 * three low sides are on and every SOx is valid. */
void Board_Tim1AdcTrigger(void);

/* Start / stop the six PWM outputs. Start sets 50 % first and sets MOE. */
void Board_Tim1StartPwm(void);
void Board_Tim1StopPwm(void);

/* Raw channel duties in [0, 1]: ch1 -> INHC, ch2 -> INHB, ch3 -> INHA.
 * 0 is exact: low side held on for the whole period, no edges. */
void Board_SetPwmRaw(float ch1, float ch2, float ch3);

/* Phase-ordered duties: A -> CH3, B -> CH2, C -> CH1. */
void Board_SetPwmPhases(float da, float db, float dc);

/* Bottom-clamped modulation (DPWMMIN). v = phase voltages, any common unit;
 * vbus in the same unit. The lowest phase gets exactly 0 (low side held on),
 * the others (v - vmin) / vbus. Linear up to |v_ll| = vbus, same as SVPWM.
 * Output order d[0..2] = A, B, C. */
void Board_DutyDpwmMin(float va, float vb, float vc, float vbus, float d[3]);

/* Safe stop from anywhere (thread or ISR): MOE = 0 so every INx goes low
 * (bridge Hi-Z), and DRVOFF low so nFAULT stays meaningful. */
void Board_BridgeOff(void);

/* Ctrl-C / ESC on the USB console calls Board_BridgeOff() and sets this flag.
 * Long-running apps should poll Board_StopRequested() and stop for good. */
void    Board_RequestStop(void);
uint8_t Board_StopRequested(void);
void    Board_ClearStop(void);          /* for interactive apps that restart */

/* ---- DRV8316 control helpers ------------------------------------------- */
void    Board_DrvSleep(void);
void    Board_DrvWake(void);
void    Board_DrvOutputEnable(void);    /* DRVOFF low (the normal state)     */
void    Board_DrvOutputDisable(void);   /* DRVOFF high: emergency kill only,
                                         * nFAULT reads low while it is held */
uint8_t Board_DrvFault(void);           /* 1 = nFAULT low (fault) */
uint8_t Board_DrvFaultDebounced(void);  /* 1 = nFAULT still low after 2 ms */
int     Board_DrvClearFault(void);      /* 30 us nSLEEP pulse + 2 ms wait   */
int     Board_DrvInit(void);

/* ---- misc --------------------------------------------------------------- */
uint32_t Board_Millis(void);
void     Board_DelayMs(uint32_t ms);    /* keeps the USB console alive */
void     Board_DelayUs(uint32_t us);    /* DWT busy-wait */
void     Board_Poll(void);              /* service USB; call in long loops */
void     Error_Handler(void);
void     HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

#endif /* BOARD_H */
