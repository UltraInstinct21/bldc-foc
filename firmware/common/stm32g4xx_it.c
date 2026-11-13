/**
 * @file    stm32g4xx_it.c
 * @brief   Cortex-M4 and peripheral interrupt handlers.
 */
#include "main.h"
#include "stm32g4xx_it.h"
#include "board.h"

/* ---- Cortex-M4 exceptions ---------------------------------------------- */
void NMI_Handler(void)
{
    for (;;) { }
}

void HardFault_Handler(void)
{
    for (;;) { }
}

void MemManage_Handler(void)
{
    for (;;) { }
}

void BusFault_Handler(void)
{
    for (;;) { }
}

void UsageFault_Handler(void)
{
    for (;;) { }
}

void SVC_Handler(void) { }
void DebugMon_Handler(void) { }
void PendSV_Handler(void) { }

void SysTick_Handler(void)
{
    HAL_IncTick();
}

/* ---- peripheral interrupts --------------------------------------------- */
void DMA1_Channel1_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&hdma_adc1);
}

void I2C1_EV_IRQHandler(void)
{
    HAL_I2C_EV_IRQHandler(&hi2c1);
}

void I2C1_ER_IRQHandler(void)
{
    HAL_I2C_ER_IRQHandler(&hi2c1);
}

void USART1_IRQHandler(void)
{
    HAL_UART_IRQHandler(&huart1);
}
