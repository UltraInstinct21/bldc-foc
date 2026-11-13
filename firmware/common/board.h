/**
 * @file    board.h
 * @brief   Pin map and peripheral bring-up for bldc_control_final_v6.
 *
 * Board: STM32G431CBT6 (LQFP-48) + DRV8316R + 3 low-side shunts.
 *
 * Every test app and the final FOC app call Board_Init() first, then only
 * the peripheral init functions they need. Nothing here starts the bridge
 * or enables the motor outputs; that is always explicit.
 *
 * Accessible pins on this board revision (from the KiCad PCB):
 *
 *   J6: 1 = 3V3, 2 = PA15, 3 = GND
 *   J7: 1 = PB6 (USART1_TX), 2 = PB7 (USART1_RX / I2C1_SDA), no GND
 *   J5: 1 = 3V3, 2 = SWDIO, 3 = SWCLK, 4 = GND
 *   J2: motor phases C / B / A
 *
 * The AS5600 is wired to the J6/J7 pins:
 *   SCL = PA15 (J6.2), SDA = PB7 (J7.2), VCC = J6.1, GND = J6.3.
 *
 * The DRV8316CT is strapped for 6x PWM and 0.3 V/A. It has no SPI link, so
 * the board layer only drives nSLEEP and DRVOFF and reads nFAULT.
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

/* ---- debug and ESP32 link: USART1 on PB6 (J7.1) -------------------------
 * Normal debug output is TX-only, push-pull. Board_InitUsart1Link() switches
 * the same pin to half-duplex open-drain for the ESP32. PB7 stays free for
 * the encoder SDA. */
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
#define PWM_DEADTIME        0x80u           /* ~753 ns */

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
 * Clock + GPIO safe state + debug UART. Leaves:
 *   nSLEEP low (DRV asleep), DRVOFF high (outputs off), nSCS high.
 */
void Board_Init(void);

/* Switch USART1 to half-duplex open-drain on PB6 for the ESP32 link.
 * Call once at startup, after Board_Init(). Requires a 4.7k pull-up on
 * the line; the debug console keeps working on the same pin. */
void Board_InitUsart1Link(uint32_t baud);

void Board_InitI2C1(void);              /* PA15/PB7, 400 kHz */
void Board_InitTim1(uint32_t arr);      /* centre-aligned PWM, 3 + 3 outputs */
void Board_InitAdc1(void);              /* PA0/PA1/PA2, TIM1_TRGO, DMA */

/* Route TIM1_CH4/OC4REF to the ADC trigger at the counter peak. */
void Board_Tim1AdcTrigger(void);

/* Start / stop the six PWM outputs. Start sets 50 % first. */
void Board_Tim1StartPwm(void);
void Board_Tim1StopPwm(void);

/* Raw channel duties in [0, 1]: ch1 -> INHC, ch2 -> INHB, ch3 -> INHA. */
void Board_SetPwmRaw(float ch1, float ch2, float ch3);

/* ---- DRV8316 control helpers ------------------------------------------- */
void    Board_DrvSleep(void);
void    Board_DrvWake(void);
void    Board_DrvOutputEnable(void);
void    Board_DrvOutputDisable(void);
uint8_t Board_DrvFault(void);           /* 1 = nFAULT low (fault) */

/* ---- misc --------------------------------------------------------------- */
uint32_t Board_Millis(void);
void     Board_DelayMs(uint32_t ms);
void     Error_Handler(void);

#endif /* BOARD_H */
