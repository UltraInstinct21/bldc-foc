/**
 * @file    stm32g4xx_hal_msp.c
 * @brief   Low-level clocks / pin AFs for bldc_control_final_v6.
 *
 * Pin map (verified against bldc_control_final_v6.kicad_pcb):
 *
 *   USART1_TX  PB6  AF7   J7.1  (debug console, TX only)
 *   I2C1_SCL   PA15 AF4   J6.2  (AS5600)
 *   I2C1_SDA   PB7  AF4   J7.2  (AS5600, shares USART1_RX)
 *   SPI1_SCK   PB3  AF5   DRV8316
 *   SPI1_MISO  PB4  AF5   DRV8316
 *   SPI1_MOSI  PB5  AF5   DRV8316
 *   TIM1_CH1   PA8  AF6 -> INHC
 *   TIM1_CH2   PA9  AF6 -> INHB
 *   TIM1_CH3   PA10 AF6 -> INHA
 *   TIM1_CH1N  PB13 AF6 -> INLC
 *   TIM1_CH2N  PB14 AF6 -> INLB
 *   TIM1_CH3N  PB15 AF4 -> INLA
 *   ADC1_IN1/2/3  PA0/PA1/PA2   SOA/SOB/SOC
 *
 * Note: SPI1 on PB3/PB4/PB5 is AF5, not AF6 (AF6 is SPI3).
 */
#include "main.h"
#include "board.h"

void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
    HAL_PWREx_DisableUCPDDeadBattery();
}

void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
    GPIO_InitTypeDef        gpio = {0};
    RCC_PeriphCLKInitTypeDef clk = {0};

    if (hadc->Instance != ADC1)
    {
        return;
    }

    clk.PeriphClockSelection = RCC_PERIPHCLK_ADC12;
    clk.Adc12ClockSelection  = RCC_ADC12CLKSOURCE_SYSCLK;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
    {
        Error_Handler();
    }

    __HAL_RCC_ADC12_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* PA0 = SOA, PA1 = SOB, PA2 = SOC */
    gpio.Pin  = GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    hdma_adc1.Instance                 = DMA1_Channel1;
    hdma_adc1.Init.Request             = DMA_REQUEST_ADC1;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode                = DMA_CIRCULAR;
    hdma_adc1.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&hdma_adc1) != HAL_OK)
    {
        Error_Handler();
    }
    __HAL_LINKDMA(hadc, DMA_Handle, hdma_adc1);

    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 1u, 0u);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
}

void HAL_I2C_MspInit(I2C_HandleTypeDef *hi2c)
{
    GPIO_InitTypeDef        gpio = {0};
    RCC_PeriphCLKInitTypeDef clk = {0};

    if (hi2c->Instance != I2C1)
    {
        return;
    }

    clk.PeriphClockSelection = RCC_PERIPHCLK_I2C1;
    clk.I2c1ClockSelection   = RCC_I2C1CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
    {
        Error_Handler();
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    /* PA15 = SCL, PB7 = SDA. Open drain; external 4.7k pull-ups required. */
    gpio.Mode      = GPIO_MODE_AF_OD;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF4_I2C1;

    gpio.Pin = GPIO_PIN_15;
    HAL_GPIO_Init(GPIOA, &gpio);
    gpio.Pin = GPIO_PIN_7;
    HAL_GPIO_Init(GPIOB, &gpio);

    HAL_NVIC_SetPriority(I2C1_EV_IRQn, 5u, 0u);
    HAL_NVIC_EnableIRQ(I2C1_EV_IRQn);
    HAL_NVIC_SetPriority(I2C1_ER_IRQn, 5u, 0u);
    HAL_NVIC_EnableIRQ(I2C1_ER_IRQn);
}

void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    GPIO_InitTypeDef gpio = {0};

    if (hspi->Instance != SPI1)
    {
        return;
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_SPI1_CLK_ENABLE();

    /* PB3 = SCK, PB4 = MISO, PB5 = MOSI. SPI1 is AF5. */
    gpio.Pin       = GPIO_PIN_3 | GPIO_PIN_4 | GPIO_PIN_5;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF5_SPI1;
    HAL_GPIO_Init(GPIOB, &gpio);
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1)
    {
        __HAL_RCC_TIM1_CLK_ENABLE();
    }
}

void HAL_TIM_PWM_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM1)
    {
        __HAL_RCC_TIM1_CLK_ENABLE();
    }
}

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim)
{
    GPIO_InitTypeDef gpio = {0};

    if (htim->Instance != TIM1)
    {
        return;
    }

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();

    /* PA8/PA9/PA10 = CH1/CH2/CH3 (AF6). */
    gpio.Pin       = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_10;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    gpio.Alternate = GPIO_AF6_TIM1;
    HAL_GPIO_Init(GPIOA, &gpio);

    /* PB13/PB14 = CH1N/CH2N (AF6). */
    gpio.Pin = GPIO_PIN_13 | GPIO_PIN_14;
    HAL_GPIO_Init(GPIOB, &gpio);

    /* PB15 = CH3N is AF4 on this package. */
    gpio.Pin       = GPIO_PIN_15;
    gpio.Alternate = GPIO_AF4_TIM1;
    HAL_GPIO_Init(GPIOB, &gpio);
}

void HAL_UART_MspInit(UART_HandleTypeDef *huart)
{
    GPIO_InitTypeDef        gpio = {0};
    RCC_PeriphCLKInitTypeDef clk = {0};

    if (huart->Instance != USART1)
    {
        return;
    }

    clk.PeriphClockSelection = RCC_PERIPHCLK_USART1;
    clk.Usart1ClockSelection = RCC_USART1CLKSOURCE_PCLK2;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
    {
        Error_Handler();
    }

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_USART1_CLK_ENABLE();

    /* TX only: PB6 AF7. PB7 is intentionally left to I2C1_SDA. */
    gpio.Pin       = GPIO_PIN_6;
    gpio.Mode      = GPIO_MODE_AF_PP;
    gpio.Pull      = GPIO_NOPULL;
    gpio.Speed     = GPIO_SPEED_FREQ_LOW;
    gpio.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOB, &gpio);
}
