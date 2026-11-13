/**
 * @file    main.h
 * @brief   Common definitions for the bldc_control_final_v6 test/foc firmware.
 */
#ifndef MAIN_H
#define MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g4xx_hal.h"

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);
void Error_Handler(void);

#ifdef __cplusplus
}
#endif

#endif /* MAIN_H */
