/**
 * @file    main.c
 * @brief   test_05_open_loop — independent open-loop V/f motor spin.
 *
 * Covers the whole power path without the FOC current loop:
 *   - SPI DRV8316 configuration (6x, 200 V/us, 0.15 V/A)
 *   - nSLEEP / DRVOFF sequencing
 *   - TIM1 three-phase sine PWM
 *   - motor phases and the optional AS5600 feedback
 *
 * SAFETY: this is open loop. There is no current loop and no speed loop.
 * The motor must be free to spin. The modulation is deliberately small
 * because this motor has Rph = 0.109 ohm: 1 % of 24 V is already ~2.2 A.
 *
 * Expected: the rotor aligns, then slowly accelerates to ~20 Hz electrical
 * (about 170 rpm mechanical for 7 pole pairs). Power down if it hums and
 * does not turn.
 */
#include "board.h"
#include "debug.h"
#include "drv8316.h"
#include "as5600.h"

#include <math.h>

#define OPENLOOP_USE_ENCODER    1
#define OPENLOOP_FMAX_HZ        20.0f
#define OPENLOOP_RAMP_S         10.0f
#define OPENLOOP_MOD_MIN        0.012f
#define OPENLOOP_MOD_PER_HZ     0.0015f
#define OPENLOOP_MOD_MAX        0.05f
#define OPENLOOP_ALIGN_MS       1000u

#ifndef PI_F
#define PI_F                    3.14159265359f
#endif
#define TWO_PI_F                (2.0f * PI_F)

/* Apply a stationary vector. mod = peak phase voltage / Vbus. */
static void apply_vector(float theta, float mod)
{
    float a = 0.5f + 0.5f * mod * sinf(theta);
    float b = 0.5f + 0.5f * mod * sinf(theta - TWO_PI_F / 3.0f);
    float c = 0.5f + 0.5f * mod * sinf(theta + TWO_PI_F / 3.0f);

    /* Board order: CH1=phase C, CH2=phase B, CH3=phase A. */
    Board_SetPwmRaw(c, b, a);
}

int main(void)
{
    float    theta = 0.0f;
    uint32_t t_start;
    uint32_t t_prev;
    uint32_t t_print;
    uint8_t  have_enc = 0u;
    uint16_t enc_prev = 0u;
    uint32_t enc_t_prev = 0u;
    float    rpm = 0.0f;
    uint8_t  status = 0u;

    Board_Init();
    Board_InitTim1(PWM_ARR_20KHZ);
    Board_Tim1StartPwm();          /* 50 % first, MOE on, DRVOFF still high */

    DBG_Puts("\r\n\r\n=== test_05_open_loop (V/f) ===\r\n");

#if OPENLOOP_USE_ENCODER
    Board_InitI2C1();
    if (AS5600_Init(&hi2c1, &status) == 0)
    {
        have_enc = 1u;
        (void)AS5600_ReadBlocking(&enc_prev, 20u);
        enc_t_prev = Board_Millis();
        DBG_Printf("AS5600 OK (STATUS=0x%02X), angle telemetry enabled.\r\n", status);
    }
    else
    {
        DBG_Puts("AS5600 not found; running without angle telemetry.\r\n");
    }
#endif

    Board_DrvSleep();
    Board_DelayMs(2u);
    Board_DrvWake();
    Board_DelayMs(5u);

    if (DRV8316_Init() != 0)
    {
        DBG_Puts("DRV8316_Init failed - not enabling the bridge.\r\n");
        Board_Tim1StopPwm();
        for (;;) { Board_DelayMs(1000u); }
    }
    DBG_Puts("DRV8316CT ready: 6x PWM, 125 V/us, CSA 0.3 V/A (hardware straps).\r\n");

    /* Align at electrical 0. */
    DBG_Printf("Aligning for %u ms at mod=%.3f...\r\n",
               (unsigned)OPENLOOP_ALIGN_MS, (double)OPENLOOP_MOD_MIN);
    apply_vector(0.0f, OPENLOOP_MOD_MIN);
    Board_DrvOutputEnable();
    Board_DelayMs(OPENLOOP_ALIGN_MS);

    DBG_Puts("Ramping frequency. Motor should now turn.\r\n\r\n");
    DBG_Puts("   t_ms    fe_Hz   mod    enc_deg    rpm   nFAULT\r\n");

    t_start = Board_Millis();
    t_prev  = t_start;
    t_print = t_start;

    for (;;)
    {
        uint32_t now = Board_Millis();
        float    dt  = (float)(now - t_prev) * 0.001f;
        float    t   = (float)(now - t_start) * 0.001f;
        float    fe;
        float    mod;

        if (dt <= 0.0f)
        {
            dt = 0.001f;
        }
        t_prev = now;

        fe = OPENLOOP_FMAX_HZ * (t / OPENLOOP_RAMP_S);
        if (fe > OPENLOOP_FMAX_HZ)
        {
            fe = OPENLOOP_FMAX_HZ;
        }

        mod = OPENLOOP_MOD_MIN + OPENLOOP_MOD_PER_HZ * fe;
        if (mod > OPENLOOP_MOD_MAX)
        {
            mod = OPENLOOP_MOD_MAX;
        }

        theta += TWO_PI_F * fe * dt;
        if (theta >= TWO_PI_F)
        {
            theta -= TWO_PI_F;
        }

        apply_vector(theta, mod);

        if (Board_DrvFault() != 0u)
        {
            DBG_Puts("nFAULT asserted: disabling the bridge.\r\n");
            Board_DrvOutputDisable();
            Board_Tim1StopPwm();
            for (;;) { Board_DelayMs(1000u); }
        }

        if ((now - t_print) >= 250u)
        {
            t_print = now;

#if OPENLOOP_USE_ENCODER
            if (have_enc != 0u)
            {
                uint16_t raw = 0u;
                if (AS5600_ReadBlocking(&raw, 20u) == 0)
                {
                    int32_t d = (int32_t)raw - (int32_t)enc_prev;
                    uint32_t edt = now - enc_t_prev;

                    if (d > 2048)       { d -= 4096; }
                    else if (d < -2048) { d += 4096; }

                    if (edt > 0u)
                    {
                        rpm = (float)d * (60.0f / 4096.0f) * (1000.0f / (float)edt);
                    }
                    enc_prev   = raw;
                    enc_t_prev = now;
                }
            }
#endif

            DBG_Printf("%7lu   %5.2f  %5.3f   %8.1f  %6.1f   %u\r\n",
                       (unsigned long)(now - t_start),
                       (double)fe,
                       (double)mod,
                       (double)((float)enc_prev * 360.0f / 4096.0f),
                       (double)rpm,
                       (unsigned)Board_DrvFault());
        }

        Board_DelayMs(1u);
    }
}
