/**
 * @file    board.c
 * @brief   Clock, GPIO and peripheral init for bldc_control_final_v6.
 *
 * Low-level clock enables and pin AFs live in stm32g4xx_hal_msp.c; this
 * file only fills in the HAL handle Init structures and provides the small
 * board-level helpers the tests use.
 */
#include "board.h"
#include "debug.h"
#include "main.h"

/* HAL handles, shared by every app. */
UART_HandleTypeDef huart1;
I2C_HandleTypeDef  hi2c1;
TIM_HandleTypeDef  htim1;
ADC_HandleTypeDef  hadc1;
DMA_HandleTypeDef  hdma_adc1;

#define PLL_M           4u
#define PLL_N           85u
#define PLL_R           2u

static void clock_config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};

    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

    osc.OscillatorType      = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState            = RCC_HSI_ON;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    osc.PLL.PLLState        = RCC_PLL_ON;
    osc.PLL.PLLSource       = RCC_PLLSOURCE_HSI;
    osc.PLL.PLLM            = RCC_PLLM_DIV4;
    osc.PLL.PLLN            = PLL_N;
    osc.PLL.PLLP            = RCC_PLLP_DIV2;
    osc.PLL.PLLQ            = RCC_PLLQ_DIV2;
    osc.PLL.PLLR            = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        Error_Handler();
    }

    clk.ClockType      = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK |
                         RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clk.SYSCLKSource   = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider  = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    clk.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_4) != HAL_OK)
    {
        Error_Handler();
    }
}

static void gpio_config(void)
{
    GPIO_InitTypeDef g = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Safe defaults before anything becomes an output. */
    HAL_GPIO_WritePin(DRV_NSLEEP_PORT, DRV_NSLEEP_PIN, GPIO_PIN_RESET); /* asleep */
    HAL_GPIO_WritePin(DRV_DRVOFF_PORT,  DRV_DRVOFF_PIN,  GPIO_PIN_SET); /* outputs off */

    /* nSLEEP: push-pull, starts low. */
    g.Pin   = DRV_NSLEEP_PIN;
    g.Mode  = GPIO_MODE_OUTPUT_PP;
    g.Pull  = GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(DRV_NSLEEP_PORT, &g);

    /* DRVOFF: push-pull, starts high. */
    g.Pin = DRV_DRVOFF_PIN;
    HAL_GPIO_Init(DRV_DRVOFF_PORT, &g);

    /* nFAULT: open-drain, active low, needs a pull-up. */
    g.Pin  = DRV_NFAULT_PIN;
    g.Mode = GPIO_MODE_INPUT;
    g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(DRV_NFAULT_PORT, &g);
}

void Board_Init(void)
{
    HAL_Init();
    clock_config();
    gpio_config();

    /* USART1 TX-only on PB6 for the first banner. PB7 is left free for the
     * encoder SDA. Board_InitUsart1Link() upgrades this to half-duplex. */
    huart1.Instance          = USART1;
    huart1.Init.BaudRate     = DBG_UART_BAUD;
    huart1.Init.WordLength   = UART_WORDLENGTH_8B;
    huart1.Init.StopBits     = UART_STOPBITS_1;
    huart1.Init.Parity       = UART_PARITY_NONE;
    huart1.Init.Mode         = UART_MODE_TX;
    huart1.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling = UART_OVERSAMPLING_16;
    huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        Error_Handler();
    }
}

void Board_InitI2C1(void)
{
    hi2c1.Instance              = I2C1;
    hi2c1.Init.Timing           = 0x40B285C2;   /* 400 kHz at 170 MHz PCLK1 */
    hi2c1.Init.OwnAddress1      = 0;
    hi2c1.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2      = 0;
    hi2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c1.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    {
        Error_Handler();
    }
    (void)HAL_I2CEx_ConfigAnalogFilter(&hi2c1, I2C_ANALOGFILTER_ENABLE);
    (void)HAL_I2CEx_ConfigDigitalFilter(&hi2c1, 0);
}

void Board_InitUsart1Link(uint32_t baud)
{
    GPIO_InitTypeDef g = {0};

    /* Re-init the handle in TX+RX mode at the link baud rate. */
    (void)HAL_UART_DeInit(&huart1);

    huart1.Instance            = USART1;
    huart1.Init.BaudRate       = baud;
    huart1.Init.WordLength     = UART_WORDLENGTH_8B;
    huart1.Init.StopBits       = UART_STOPBITS_1;
    huart1.Init.Parity         = UART_PARITY_NONE;
    huart1.Init.Mode           = UART_MODE_TX_RX;
    huart1.Init.HwFlowCtl      = UART_HWCONTROL_NONE;
    huart1.Init.OverSampling   = UART_OVERSAMPLING_16;
    huart1.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart1.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    huart1.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart1) != HAL_OK)
    {
        Error_Handler();
    }

    /* PB6 is the shared line. Open-drain lets the ESP32 drive it as well;
     * the external 4.7k pull-up provides the idle high level. */
    g.Pin       = GPIO_PIN_6;
    g.Mode      = GPIO_MODE_AF_OD;
    g.Pull      = GPIO_PULLUP;
    g.Speed     = GPIO_SPEED_FREQ_VERY_HIGH;
    g.Alternate = GPIO_AF7_USART1;
    HAL_GPIO_Init(GPIOB, &g);

    /* Half-duplex: TX pin is the bidirectional line, RX pin is unused. */
    SET_BIT(huart1.Instance->CR3, USART_CR3_HDSEL);

    HAL_NVIC_SetPriority(USART1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(USART1_IRQn);
}

void Board_InitTim1(uint32_t arr)
{
    TIM_MasterConfigTypeDef        master = {0};
    TIM_OC_InitTypeDef             oc = {0};
    TIM_BreakDeadTimeConfigTypeDef bd = {0};

    htim1.Instance               = TIM1;
    htim1.Init.Prescaler         = 0;
    htim1.Init.CounterMode       = TIM_COUNTERMODE_CENTERALIGNED1;
    htim1.Init.Period            = arr;
    htim1.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
    {
        Error_Handler();
    }

    master.MasterOutputTrigger  = TIM_TRGO_UPDATE;
    master.MasterOutputTrigger2 = TIM_TRGO2_RESET;
    master.MasterSlaveMode      = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &master) != HAL_OK)
    {
        Error_Handler();
    }

    oc.OCMode       = TIM_OCMODE_PWM1;
    oc.Pulse        = 0;
    oc.OCPolarity   = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity  = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode   = TIM_OCFAST_DISABLE;
    oc.OCIdleState  = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    if ((HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_1) != HAL_OK) ||
        (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_2) != HAL_OK) ||
        (HAL_TIM_PWM_ConfigChannel(&htim1, &oc, TIM_CHANNEL_3) != HAL_OK))
    {
        Error_Handler();
    }

    bd.OffStateRunMode  = TIM_OSSR_ENABLE;
    bd.OffStateIDLEMode = TIM_OSSI_ENABLE;
    bd.LockLevel        = TIM_LOCKLEVEL_OFF;
    bd.DeadTime         = PWM_DEADTIME;
    bd.BreakState       = TIM_BREAK_DISABLE;
    bd.BreakPolarity    = TIM_BREAKPOLARITY_HIGH;
    bd.BreakFilter      = 0;
    bd.BreakAFMode      = TIM_BREAK_AFMODE_INPUT;
    bd.Break2State      = TIM_BREAK2_DISABLE;
    bd.Break2Polarity   = TIM_BREAK2POLARITY_HIGH;
    bd.Break2Filter     = 0;
    bd.Break2AFMode     = TIM_BREAK_AFMODE_INPUT;
    bd.AutomaticOutput  = TIM_AUTOMATICOUTPUT_DISABLE;
    if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &bd) != HAL_OK)
    {
        Error_Handler();
    }

    HAL_TIM_MspPostInit(&htim1);
}

void Board_Tim1AdcTrigger(void)
{
    /* CH4 drives nothing; it only makes a rising OC4REF at the counter peak.
     * CCR4 = ARR-1 lands one count before the peak on the up-count. */
    htim1.Instance->CCER  &= ~TIM_CCER_CC4E;
    htim1.Instance->CCMR2  = (htim1.Instance->CCMR2 & ~TIM_CCMR2_OC4M_Msk) |
                             TIM_CCMR2_OC4M_2 | TIM_CCMR2_OC4M_1 | TIM_CCMR2_OC4M_0;
    htim1.Instance->CCR4   = htim1.Init.Period - 1u;
    htim1.Instance->CR2    = (htim1.Instance->CR2 & ~TIM_CR2_MMS_Msk) |
                             TIM_TRGO_OC4REF;
}

void Board_Tim1StartPwm(void)
{
    uint32_t half = htim1.Init.Period / 2u;

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, half);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, half);
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, half);

    (void)HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
    (void)HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_3);
    (void)HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_1);
    (void)HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_2);
    (void)HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_3);
}

void Board_Tim1StopPwm(void)
{
    __HAL_TIM_MOE_DISABLE(&htim1);
    (void)HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_2);
    (void)HAL_TIM_PWM_Stop(&htim1, TIM_CHANNEL_3);
    (void)HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_1);
    (void)HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_2);
    (void)HAL_TIMEx_PWMN_Stop(&htim1, TIM_CHANNEL_3);
}

void Board_SetPwmRaw(float ch1, float ch2, float ch3)
{
    uint32_t period = htim1.Init.Period + 1u;

    if (ch1 < 0.0f) { ch1 = 0.0f; }
    if (ch1 > 1.0f) { ch1 = 1.0f; }
    if (ch2 < 0.0f) { ch2 = 0.0f; }
    if (ch2 > 1.0f) { ch2 = 1.0f; }
    if (ch3 < 0.0f) { ch3 = 0.0f; }
    if (ch3 > 1.0f) { ch3 = 1.0f; }

    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_1, (uint32_t)(ch1 * (float)period));
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_2, (uint32_t)(ch2 * (float)period));
    __HAL_TIM_SET_COMPARE(&htim1, TIM_CHANNEL_3, (uint32_t)(ch3 * (float)period));
}

void Board_InitAdc1(void)
{
    ADC_ChannelConfTypeDef ch = {0};

    hadc1.Instance                      = ADC1;
    hadc1.Init.ClockPrescaler           = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution               = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign                = ADC_DATAALIGN_RIGHT;
    hadc1.Init.GainCompensation         = 0;
    hadc1.Init.ScanConvMode             = ADC_SCAN_ENABLE;
    hadc1.Init.EOCSelection             = ADC_EOC_SEQ_CONV;
    hadc1.Init.LowPowerAutoWait         = DISABLE;
    hadc1.Init.ContinuousConvMode       = DISABLE;
    hadc1.Init.NbrOfConversion          = 3;
    hadc1.Init.DiscontinuousConvMode    = DISABLE;
    hadc1.Init.ExternalTrigConv         = ADC_EXTERNALTRIG_T1_TRGO;
    hadc1.Init.ExternalTrigConvEdge     = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.DMAContinuousRequests    = ENABLE;
    hadc1.Init.Overrun                  = ADC_OVR_DATA_OVERWRITTEN;
    hadc1.Init.OversamplingMode         = DISABLE;
    if (HAL_ADC_Init(&hadc1) != HAL_OK)
    {
        Error_Handler();
    }

    ch.SamplingTime  = ADC_SAMPLETIME_6CYCLES_5;
    ch.SingleDiff    = ADC_SINGLE_ENDED;
    ch.OffsetNumber  = ADC_OFFSET_NONE;
    ch.Offset        = 0;

    ch.Channel = ADC_CHANNEL_1;     /* PA0 / SOA */
    ch.Rank    = ADC_REGULAR_RANK_1;
    if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) { Error_Handler(); }

    ch.Channel = ADC_CHANNEL_2;     /* PA1 / SOB */
    ch.Rank    = ADC_REGULAR_RANK_2;
    if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) { Error_Handler(); }

    ch.Channel = ADC_CHANNEL_3;     /* PA2 / SOC */
    ch.Rank    = ADC_REGULAR_RANK_3;
    if (HAL_ADC_ConfigChannel(&hadc1, &ch) != HAL_OK) { Error_Handler(); }

    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK)
    {
        Error_Handler();
    }
}

void Board_DrvSleep(void)
{
    HAL_GPIO_WritePin(DRV_NSLEEP_PORT, DRV_NSLEEP_PIN, GPIO_PIN_RESET);
}

void Board_DrvWake(void)
{
    HAL_GPIO_WritePin(DRV_NSLEEP_PORT, DRV_NSLEEP_PIN, GPIO_PIN_SET);
}

void Board_DrvOutputEnable(void)
{
    HAL_GPIO_WritePin(DRV_DRVOFF_PORT, DRV_DRVOFF_PIN, GPIO_PIN_RESET);
}

void Board_DrvOutputDisable(void)
{
    HAL_GPIO_WritePin(DRV_DRVOFF_PORT, DRV_DRVOFF_PIN, GPIO_PIN_SET);
}

uint8_t Board_DrvFault(void)
{
    return (HAL_GPIO_ReadPin(DRV_NFAULT_PORT, DRV_NFAULT_PIN) == GPIO_PIN_RESET) ? 1u : 0u;
}

uint32_t Board_Millis(void)
{
    return HAL_GetTick();
}

void Board_DelayMs(uint32_t ms)
{
    HAL_Delay(ms);
}

void Error_Handler(void)
{
    __disable_irq();
    for (;;)
    {
    }
}
