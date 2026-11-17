/**
 * @file    main.c
 * @brief   test_09_drv_diag — DRV8316 "why is nFAULT low?" diagnostics.
 *
 * Port of the bring-up "diag" command. Reads ANALOG voltages with the ADC so
 * the cases can be told apart without a meter:
 *
 *   3V3 rail       from VREFINT (factory calibration at 3.0 V)
 *   nFAULT         PA6 as ADC2_IN3 (R6 5.1k pull-up to 3V3 on the board)
 *   SOA/SOB/SOC    PA0/PA1/PA2, ADC1_IN1..3; VREF/2 = ~1.65 V when awake
 *
 * Sequence: asleep -> wake (time nFAULT) -> 30 us nSLEEP pulse -> sleep 1 s /
 * wake -> DRVOFF high 50 ms / low. Then a verdict. Repeats every 5 s so a
 * supply or rework change shows up live.
 *
 * Verdict hints:
 *   nFAULT high, SOx ~1.65 V, DRVOFF toggles nFAULT   -> driver OK
 *   nFAULT at a few hundred mV, ignores nSLEEP         -> DRV unpowered (VM)
 *   nFAULT hard 0 V, ignores everything                -> unpowered, or the
 *                                                         nFAULT net is shorted
 *   SOx 0 V while awake                                 -> DRV not running
 *                                                         (VM, AVDD, reset)
 *   SOx ~0.8 V or ~2.5 V at 0 A                         -> check GAIN/VREF pins
 */
#include "board.h"
#include "debug.h"

#define VREFINT_CAL     (*(const uint16_t *)0x1FFF75AAu)   /* raw at 3.0 V */

static void adc_enable(ADC_TypeDef *a)
{
    a->CR &= ~ADC_CR_DEEPPWD;
    a->CR |= ADC_CR_ADVREGEN;
    Board_DelayUs(30u);
    a->CR &= ~ADC_CR_ADCALDIF;
    a->CR |= ADC_CR_ADCAL;
    while ((a->CR & ADC_CR_ADCAL) != 0u) { }
    Board_DelayUs(5u);
    a->ISR = ADC_ISR_ADRDY;
    a->CR |= ADC_CR_ADEN;
    while ((a->ISR & ADC_ISR_ADRDY) == 0u) { }
}

static void adc_init(void)
{
    __HAL_RCC_ADC12_CLK_ENABLE();
    ADC12_COMMON->CCR = (ADC12_COMMON->CCR & ~ADC_CCR_CKMODE) | ADC_CCR_CKMODE_0 | ADC_CCR_CKMODE_1; /* HCLK/4 */
    ADC12_COMMON->CCR |= ADC_CCR_VREFEN;
    adc_enable(ADC1);
    adc_enable(ADC2);
}

/* one conversion, long sample time (640.5 cycles), averaged 16 times */
static float adc_read(ADC_TypeDef *a, uint32_t ch)
{
    uint32_t sum = 0u;

    a->SQR1 = (ch << ADC_SQR1_SQ1_Pos);              /* L = 0: one conversion */
    if (ch < 10u) { a->SMPR1 = (a->SMPR1 & ~(7u << (3u * ch))) | (7u << (3u * ch)); }
    else          { a->SMPR2 = (a->SMPR2 & ~(7u << (3u * (ch - 10u)))) | (7u << (3u * (ch - 10u))); }
    for (uint32_t n = 0u; n < 16u; n++)
    {
        a->ISR = ADC_ISR_EOC;
        a->CR |= ADC_CR_ADSTART;
        while ((a->ISR & ADC_ISR_EOC) == 0u) { }
        sum += a->DR;
    }
    return (float)sum / 16.0f;
}

static float vdda(void)
{
    return 3.0f * (float)VREFINT_CAL / adc_read(ADC1, 18u);
}

static float nfault_volts(float vref)
{
    GPIO_InitTypeDef g = { 0 };
    float v;

    g.Pin = DRV_NFAULT_PIN; g.Mode = GPIO_MODE_ANALOG; g.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(DRV_NFAULT_PORT, &g);
    v = vref * adc_read(ADC2, 3u) / 4095.0f;
    g.Mode = GPIO_MODE_INPUT; g.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(DRV_NFAULT_PORT, &g);
    return v;
}

static void sox(float vref, float v[3])
{
    for (uint32_t k = 0u; k < 3u; k++) { v[k] = vref * adc_read(ADC1, k + 1u) / 4095.0f; }
}

/* ms nFAULT stays low, up to max_ms; 0 = never went low within 5 ms */
static uint32_t watch_low_us(uint32_t max_us)
{
    uint32_t cpu = SystemCoreClock / 1000000u;
    uint32_t t0 = DWT->CYCCNT, tf;

    while (Board_DrvFault() == 0u)
    {
        if ((DWT->CYCCNT - t0) > 5000u * cpu) { return 0u; }
    }
    tf = DWT->CYCCNT;
    while ((Board_DrvFault() != 0u) && ((DWT->CYCCNT - tf) < max_us * cpu)) { }
    return (DWT->CYCCNT - tf) / cpu;
}

static void report(const char *what, float vref)
{
    float s[3];
    sox(vref, s);
    printf("  %-26s nFAULT %s (%.2f V) | SOA %.3f SOB %.3f SOC %.3f V\r\n", what,
           Board_DrvFault() ? "LOW " : "high", (double)nfault_volts(vref),
           (double)s[0], (double)s[1], (double)s[2]);
}

int main(void)
{
    Board_Init();
    adc_init();

    for (;;)
    {
        float vref = vdda(), nf_sleep, nf_awake, s[3];
        uint32_t wake_us, sleep_us;
        uint8_t clr_ok, drvoff_hi, drvoff_lo;

        printf("\r\n=== test_09_drv_diag ===  3V3 rail (VDDA) = %.3f V\r\n", (double)vref);

        Board_DrvOutputEnable();                     /* DRVOFF low */
        Board_DrvSleep();
        Board_DelayMs(20u);
        nf_sleep = nfault_volts(vref);
        report("asleep (nSLEEP low):", vref);

        Board_DrvWake();
        wake_us = watch_low_us(300000u);
        Board_DelayMs(5u);
        nf_awake = nfault_volts(vref);
        report("awake 5 ms after wake:", vref);
        printf("  nFAULT low for %lu us after wake (healthy: ~1700-1800 us)\r\n", (unsigned long)wake_us);

        clr_ok = (DRV8316_ClearFault() == 0) ? 1u : 0u;
        printf("  30 us nSLEEP pulse: nFAULT %s\r\n", clr_ok ? "high (cleared / OK)" : "still LOW");

        Board_DrvSleep();
        Board_DelayMs(1000u);
        Board_DrvWake();
        sleep_us = watch_low_us(300000u);
        Board_DelayMs(5u);
        printf("  1 s sleep -> wake: nFAULT low for %lu us, now %s\r\n",
               (unsigned long)sleep_us, Board_DrvFault() ? "LOW" : "high");

        Board_DrvOutputDisable();
        Board_DelayMs(50u);
        drvoff_hi = Board_DrvFault();
        Board_DrvOutputEnable();
        Board_DelayMs(20u);
        drvoff_lo = Board_DrvFault();
        printf("  DRVOFF high: nFAULT %s (expect LOW) | DRVOFF low: nFAULT %s (expect high)\r\n",
               drvoff_hi ? "LOW" : "high", drvoff_lo ? "LOW" : "high");

        sox(vref, s);
        printf("VERDICT: ");
        if ((drvoff_lo == 0u) && (drvoff_hi != 0u))
        {
            float m = (s[0] + s[1] + s[2]) / 3.0f;
            printf("driver OK. SOx mean %.3f V (expect ~%.2f V = VREF/2)%s\r\n", (double)m,
                   (double)(vref * 0.5f),
                   ((m < vref * 0.5f - 0.15f) || (m > vref * 0.5f + 0.15f)) ? " <- offset off: check GAIN/VREF pins" : "");
        }
        else if (Board_DrvFault() == 0u)
        {
            printf("nFAULT high but does not follow DRVOFF: check the DRVOFF net (pin 21, PA7)\r\n");
        }
        else if (nf_sleep > 2.5f)
        {
            printf("nFAULT net OK (high while asleep), but the DRV asserts a fault on every wake\r\n"
                   "         that the nSLEEP pulse does not clear: a persistent supply-side fault.\r\n"
                   "         Check VM (pins 9-11), CP (pin 8, ~VM+5 V), AVDD (pin 25), FB_BK (pin 3).\r\n");
            for (uint32_t k = 0u; k < 3u; k++)
            {
                if ((s[k] < 0.5f * vref - 0.3f) || (s[k] > 0.5f * vref + 0.3f))
                {
                    printf("         SO%c = %.3f V awake (expect ~%.2f V): output loaded or shorted\r\n",
                           (int)('A' + (int)k), (double)s[k], (double)(0.5f * vref));
                }
            }
        }
        else if (nf_awake < 0.05f)
        {
            printf("nFAULT is a hard 0 V and ignores nSLEEP/DRVOFF: DRV unpowered (VM) or the\r\n"
                   "         nFAULT net (pin 22, PA6, R6) is shorted to GND. SOx %.2f/%.2f/%.2f V.\r\n",
                   (double)s[0], (double)s[1], (double)s[2]);
        }
        else
        {
            printf("nFAULT stuck at %.2f V and ignores nSLEEP: DRV not running. No VM on pins 9-11,\r\n"
                   "         or AVDD (pin 25) / charge pump (pins 6-8) shorted. SOx %.2f/%.2f/%.2f V.\r\n",
                   (double)nf_awake, (double)s[0], (double)s[1], (double)s[2]);
        }
        (void)nf_sleep;
        Board_DelayMs(5000u);
    }
}
