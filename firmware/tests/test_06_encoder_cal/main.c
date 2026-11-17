/**
 * @file    main.c
 * @brief   test_06_encoder_cal — one-time encoder sign/offset calibration.
 *
 * Covers:
 *   - AS5600 + I2C1
 *   - DRV8316T (Hardware Mode)
 *   - TIM1 open-loop alignment vector, bottom-clamped (DPWMMIN): with
 *     DRV8316 pin 35 floating a vector made with all three phases switching
 *     passes almost no current, so the lowest phase is held at 0 %
 *   - Encoder_Calibrate() sign/offset measurement and flash persistence
 *
 * The rotor must be free to move. The alignment modulation is 0.025 (see
 * CAL_MOD below for why it cannot be smaller with a 1 us dead time).
 *
 * Expected: the rotor twitches to three electrical positions, then the
 * console prints sign = +/-1, offset in radians, and "saved".
 */
#include "board.h"
#include "debug.h"
#include "as5600.h"
#include "encoder.h"

#include <math.h>

#ifndef PI_F
#define PI_F                    3.14159265359f
#endif
#define TWO_PI_F                (2.0f * PI_F)

/* Alignment modulation for the encoder calibration (peak phase V / Vbus).
 * The 1 us dead time sets a floor: with DPWMMIN the vector at theta = 0
 * switches only phase A, at duty 1.5 * mod, and any pulse under 1 us (2 %)
 * is swallowed by the dead time. 0.01 gave no current at all. Effective
 * duty is about 1.5 * mod - 0.02:
 *   0.025 -> 1.75 % -> ~2.6 A on the 0.109 ohm motor at 24 V
 *                      ~0.4 A on the bring-up drone motor at 9 V
 * Raise it with USER_DEFS (and FOC_ALIGN_MOD_MAX) if the rotor does not
 * follow (rc = -3); 0.05 is about 0.8-1 A on the drone motor at 9 V. */
#ifndef CAL_MOD
#define CAL_MOD                 0.025f
#endif
#define CAL_HOLD_MS             400u

/* mod = peak phase voltage / Vbus. */
static void apply_vector(float theta, float mod)
{
    float d[3];

    Board_DutyDpwmMin(mod * cosf(theta),
                      mod * cosf(theta - TWO_PI_F / 3.0f),
                      mod * cosf(theta + TWO_PI_F / 3.0f),
                      1.0f, d);
    Board_SetPwmPhases(d[0], d[1], d[2]);
}

int main(void)
{
    uint8_t status = 0u;
    int     rc;

    Board_Init();
    Board_InitI2C1();
    Board_InitTim1(PWM_ARR_20KHZ);
    Board_Tim1StartPwm();

    DBG_Puts("\r\n\r\n=== test_06_encoder_cal ===\r\n");

    if (AS5600_Init(&hi2c1, &status) != 0)
    {
        DBG_Puts("AS5600 not found. Calibration cannot run.\r\n");
        for (;;) { Board_DelayMs(1000u); }
    }
    DBG_Printf("AS5600 STATUS=0x%02X\r\n", status);

    if ((status & AS5600_ST_MD) == 0u)
    {
        DBG_Puts("No magnet detected - fix the magnet before calibrating.\r\n");
    }

    Encoder_Init();
    DBG_Printf("Existing calibration valid=%u\r\n",
               (unsigned)Encoder_GetCal()->valid);

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

    Board_DrvOutputEnable();
    DBG_Puts("Rotor must be free. Aligning...\r\n");

    rc = Encoder_Calibrate(apply_vector, CAL_HOLD_MS, CAL_MOD);

    Board_Tim1StopPwm();
    Board_BridgeOff();

    DBG_Printf("Encoder_Calibrate rc=%d\r\n", rc);
    if (rc == 0)
    {
        DBG_Printf("  sign   = %+.0f\r\n", (double)Encoder_GetCal()->sign);
        DBG_Printf("  offset = %.4f rad (%.2f deg elec)\r\n",
                   (double)Encoder_GetCal()->offset,
                   (double)(Encoder_GetCal()->offset * 180.0f / PI_F));
        DBG_Printf("  pole_pairs = %u, valid = %u\r\n",
                   (unsigned)Encoder_GetCal()->pole_pairs,
                   (unsigned)Encoder_GetCal()->valid);
        DBG_Puts("Saved to the last flash page (0x0801F800).\r\n");
    }
    else
    {
        DBG_Puts("  -1 bad callback, -2 AS5600 read failed,\r\n");
        DBG_Puts("  -3 rotor did not follow (mod too small / phase open).\r\n");
        DBG_Puts("  Check phase wiring and the magnet, then power-cycle to retry.\r\n");
    }

    DBG_Puts("\r\nThe bridge is now off. Power-cycle (SW4 reset does not work)\r\n");
    DBG_Puts("to run calibration again,\r\n");
    DBG_Puts("or flash the FOC app to use this calibration.\r\n");

    for (;;)
    {
        Board_DelayMs(1000u);
    }
}
