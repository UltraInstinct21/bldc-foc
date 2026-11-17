/**
 * @file    main.c
 * @brief   test_03_pwm — TIM1 6-channel PWM / gate-driver input test.
 *
 * Covers:
 *   - TIM1 centre-aligned 20 kHz on PA8/PA9/PA10 (CH1/2/3)
 *   - complementary outputs on PB13/PB14 (CH1N/CH2N) and PB15 (CH3N)
 *   - the board phase mapping CH1->INHC, CH2->INHB, CH3->INHA
 *   - 120-degree three-phase modulation, bottom-clamped (DPWMMIN)
 *
 * Scope INHA/INHB/INHC (DRV pins) and the complementary INLA/INLB/INLC.
 * The DRV8316 stays asleep (nSLEEP low) by default, so the power stage is
 * Hi-Z and the motor will not move. Set PWM_TEST_ENABLE_OUTPUT to 1 only
 * with the motor free and a current-limited supply.
 *
 * Modulation is DPWMMIN, not plain sine: with OCP/SR (pin 35) floating the
 * DRV8316 passes almost no current when all three phases switch. In each
 * 120-degree sector one phase sits at 0 % (INH low, INL high, no edges).
 *
 * Expected: 20 kHz carrier, three envelopes 120 degrees apart, one phase
 * clamped low at any time, each high side and its low side complementary
 * with ~1.0 us dead time (SLEW = 125 V/us needs 1 us in 6x PWM mode).
 */
#include "board.h"
#include "debug.h"

#include <math.h>

#define PWM_TEST_ENABLE_OUTPUT  0
#define PWM_TEST_ELEC_HZ        0.5f
#define PWM_TEST_MOD            0.25f     /* phase voltage / Vbus */

#ifndef PI_F
#define PI_F                    3.14159265359f
#endif
#define TWO_PI_F                (2.0f * PI_F)

int main(void)
{
    float    theta = 0.0f;
    uint32_t t0;
    uint32_t t_print = 0u;

    Board_Init();
    Board_InitTim1(PWM_ARR_20KHZ);

    DBG_Puts("\r\n\r\n=== test_03_pwm (TIM1 20 kHz) ===\r\n");
    DBG_Printf("ARR=%lu  fsw=%lu Hz  mod=%.2f  fe=%.1f Hz\r\n",
               (unsigned long)PWM_ARR_20KHZ,
               (unsigned long)(170000000UL / (2UL * (PWM_ARR_20KHZ + 1UL))),
               (double)PWM_TEST_MOD,
               (double)PWM_TEST_ELEC_HZ);
    DBG_Puts("CH1=INHC CH2=INHB CH3=INHA, complements on CHxN, 1 us dead time.\r\n");
    DBG_Puts("DRV8316 asleep: OUTx stay Hi-Z.\r\n\r\n");

    Board_Tim1StartPwm();

#if PWM_TEST_ENABLE_OUTPUT
    Board_DrvWake();
    if (DRV8316_Init() != 0)
    {
        DBG_Puts("DRV8316 fault after wake: not enabling the bridge.\r\n");
        Board_Tim1StopPwm();
        for (;;) { Board_DelayMs(1000u); }
    }
    Board_DrvOutputEnable();
    DBG_Puts("DRV awake, DRVOFF LOW: power stage enabled.\r\n");
#endif

    t0 = Board_Millis();

    for (;;)
    {
        float d[3];
        float a, b, c;
        uint32_t t = Board_Millis();

        theta = TWO_PI_F * PWM_TEST_ELEC_HZ * (float)(t - t0) / 1000.0f;
        if (theta > TWO_PI_F)
        {
            theta -= TWO_PI_F;
        }

        Board_DutyDpwmMin(PWM_TEST_MOD * sinf(theta),
                          PWM_TEST_MOD * sinf(theta - TWO_PI_F / 3.0f),
                          PWM_TEST_MOD * sinf(theta + TWO_PI_F / 3.0f),
                          1.0f, d);
        a = d[0];
        b = d[1];
        c = d[2];

        Board_SetPwmPhases(a, b, c);    /* A -> CH3, B -> CH2, C -> CH1 */

        if (Board_StopRequested() != 0u)
        {
            Board_Tim1StopPwm();
            DBG_Puts("Stopped from the console.\r\n");
            for (;;) { Board_DelayMs(1000u); }
        }

        if ((t - t_print) >= 1000u)
        {
            t_print = t;
            DBG_Printf("t=%6lu ms  duty A/B/C = %5.1f%% %5.1f%% %5.1f%%  nFAULT=%u\r\n",
                       (unsigned long)t,
                       (double)(a * 100.0f),
                       (double)(b * 100.0f),
                       (double)(c * 100.0f),
                       (unsigned)Board_DrvFault());
        }

        Board_DelayMs(1u);
    }
}
