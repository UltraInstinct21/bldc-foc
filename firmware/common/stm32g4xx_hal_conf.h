/**
 * @file    stm32g4xx_hal_conf.h
 * @brief   Lean HAL configuration file for bldc_control_final_v6.
 */
#ifndef STM32G4xx_HAL_CONF_H
#define STM32G4xx_HAL_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Module Selection ---------------------------------------------------- */
#define HAL_MODULE_ENABLED
#define HAL_ADC_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_DMA_MODULE_ENABLED
#define HAL_EXTI_MODULE_ENABLED
#define HAL_FLASH_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED
#define HAL_I2C_MODULE_ENABLED
#define HAL_PWR_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_TIM_MODULE_ENABLED
#define HAL_UART_MODULE_ENABLED

/* ---- Oscillator Values --------------------------------------------------- */
#ifndef HSE_VALUE
#define HSE_VALUE               8000000UL
#endif
#ifndef HSE_STARTUP_TIMEOUT
#define HSE_STARTUP_TIMEOUT     100UL
#endif
#ifndef HSI_VALUE
#define HSI_VALUE               16000000UL
#endif
#ifndef HSI48_VALUE
#define HSI48_VALUE             48000000UL
#endif
#ifndef LSI_VALUE
#define LSI_VALUE               32000UL
#endif
#ifndef LSE_VALUE
#define LSE_VALUE               32768UL
#endif
#ifndef LSE_STARTUP_TIMEOUT
#define LSE_STARTUP_TIMEOUT     5000UL
#endif
#ifndef EXTERNAL_CLOCK_VALUE
#define EXTERNAL_CLOCK_VALUE    12288000UL
#endif

/* ---- System Configuration ------------------------------------------------ */
#define VDD_VALUE                   3300UL
#define TICK_INT_PRIORITY           15UL
#define USE_RTOS                    0U
#define PREFETCH_ENABLE             0U
#define INSTRUCTION_CACHE_ENABLE    1U
#define DATA_CACHE_ENABLE           1U

/* ---- Active HAL Module Includes ------------------------------------------ */
#include "stm32g4xx_hal_rcc.h"
#include "stm32g4xx_hal_gpio.h"
#include "stm32g4xx_hal_dma.h"
#include "stm32g4xx_hal_cortex.h"
#include "stm32g4xx_hal_adc.h"
#include "stm32g4xx_hal_exti.h"
#include "stm32g4xx_hal_flash.h"
#include "stm32g4xx_hal_i2c.h"
#include "stm32g4xx_hal_pwr.h"
#include "stm32g4xx_hal_tim.h"
#include "stm32g4xx_hal_uart.h"

/* ---- Assert Selection ---------------------------------------------------- */
#define assert_param(expr) ((void)0U)

#ifdef __cplusplus
}
#endif

#endif /* STM32G4xx_HAL_CONF_H */
