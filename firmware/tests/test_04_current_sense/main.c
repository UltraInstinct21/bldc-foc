/**
 * @file    main.c
 * @brief   test_04_current_sense — ADC1 + DMA + low-side shunt test.
 *
 * Covers:
 *   - ADC1 on PA0/PA1/PA2 (SOA/SOB/SOC)
 *   - TIM1_TRGO hardware trigger at the counter peak (20 kHz)
 *   - DMA1 Channel 1 circular transfer of 3 ranks
 *   - zero-current offset calibration and V -> A conversion
 *
 * TIM1 runs as a time base only (MOE off, no PWM outputs), so the DRV low
 * side is off and the shunts should read zero current -> ~1.65 V.
 *
 * Inject a known current (or enable the bridge in a separate test) to check
 * gain and sign. The DRV8316CT CSA is 0.3 V/A with a VREF/2 offset.
 * FOC uses i_phase = -(V_SOx - offset)/0.3.
 */
#include "board.h"
#include "debug.h"

#define CSA_V_PER_A     0.3f
#define ADC_VREF        3.3f
#define ADC_FULL        4095.0f

static volatile uint16_t g_dma[ADC_BUF_LEN];
static volatile uint16_t g_adc[ADC_BUF_LEN];
static volatile uint32_t g_seq;

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        for (uint32_t i = 0u; i < ADC_BUF_LEN; i++)
        {
            g_adc[i] = g_dma[i];
        }
        g_seq++;
    }
}

int main(void)
{
    float    off[ADC_BUF_LEN] = {0.0f, 0.0f, 0.0f};
    uint32_t start;
    uint32_t t_print;

    Board_Init();
    Board_InitTim1(PWM_ARR_20KHZ);
    Board_Tim1AdcTrigger();
    Board_InitAdc1();

    DBG_Puts("\r\n\r\n=== test_04_current_sense (ADC1 PA0/1/2) ===\r\n");
    DBG_Puts("Sampling at 20 kHz from TIM1_TRGO. MOE is off.\r\n");

    /* Counter only; do not start the PWM outputs. */
    if (HAL_TIM_Base_Start(&htim1) != HAL_OK)
    {
        DBG_Puts("TIM1 base start failed.\r\n");
    }

    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_dma, ADC_BUF_LEN) != HAL_OK)
    {
        DBG_Puts("ADC DMA start failed.\r\n");
        for (;;) { Board_DelayMs(1000u); }
    }

    /* Wait for the first samples. */
    start = Board_Millis();
    while ((g_seq < 16u) && ((Board_Millis() - start) < 100u))
    {
    }

    if (g_seq < 16u)
    {
        DBG_Puts("No ADC DMA samples. Check TIM1_TRGO -> ADC, DMA1 Ch1.\r\n");
    }

    /* Average the zero-current offsets. */
    {
        float sum[ADC_BUF_LEN] = {0.0f, 0.0f, 0.0f};
        const uint32_t n = 256u;

        for (uint32_t k = 0u; k < n; k++)
        {
            uint32_t seq = g_seq;

            while (g_seq == seq)
            {
            }
            for (uint32_t i = 0u; i < ADC_BUF_LEN; i++)
            {
                sum[i] += (float)g_adc[i];
            }
        }
        for (uint32_t i = 0u; i < ADC_BUF_LEN; i++)
        {
            off[i] = (sum[i] / (float)n) * (ADC_VREF / ADC_FULL);
        }
    }

    DBG_Printf("Offsets: SOA=%.4f V  SOB=%.4f V  SOC=%.4f V\r\n",
               (double)off[0], (double)off[1], (double)off[2]);
    DBG_Puts("(nominal 1.650 V; per-phase trim removes CSA/ADC offset)\r\n");
    DBG_Puts("\r\n  raw A/B/C        V A/B/C              I A/B/C (A)\r\n");

    t_print = Board_Millis();

    for (;;)
    {
        uint16_t raw[ADC_BUF_LEN];
        float    v[ADC_BUF_LEN];
        float    cur[ADC_BUF_LEN];

        for (uint32_t i = 0u; i < ADC_BUF_LEN; i++)
        {
            raw[i] = g_adc[i];
            v[i]   = (float)raw[i] * (ADC_VREF / ADC_FULL);
            cur[i] = (v[i] - off[i]) / CSA_V_PER_A;
        }

        if ((Board_Millis() - t_print) >= 200u)
        {
            t_print = Board_Millis();
            DBG_Printf("%4u %4u %4u   %6.3f %6.3f %6.3f   %7.2f %7.2f %7.2f\r\n",
                       raw[0], raw[1], raw[2],
                       (double)v[0], (double)v[1], (double)v[2],
                       (double)cur[0], (double)cur[1], (double)cur[2]);
        }

        Board_DelayMs(5u);
    }
}
