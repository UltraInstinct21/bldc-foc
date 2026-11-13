/**
 * @file    main.c
 * @brief   test_06_encoder_cal — one-time encoder sign/offset calibration.
 *
 * Covers:
 *   - AS5600 + I2C1
 *   - DRV8316CT (Hardware Mode)
 *   - TIM1 open-loop alignment vector
 *   - Encoder_Calibrate() sign/offset measurement and flash persistence
 *
 * The rotor must be free to move. The alignment modulation is 0.01, which
 * is ~2.2 A at standstill for this 0.109 ohm motor. Do not raise it.
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

#define CAL_MOD                 0.01f
#define CAL_HOLD_MS             400u

static void apply_vector(float theta, float mod)
{
    float a = 0.5f + 0.5f * mod * sinf(theta);
    float b = 0.5f + 0.5f * mod * sinf(theta - TWO_PI_F / 3.0f);
    float c = 0.5f + 0.5f * mod * sinf(theta + TWO_PI_F / 3.0f);

    Board_SetPwmRaw(c, b, a);
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

    Board_DrvOutputDisable();
    Board_Tim1StopPwm();

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
        DBG_Puts("  Check phase wiring and the magnet, then reset to retry.\r\n");
    }

    DBG_Puts("\r\nThe bridge is now off. Reset to run calibration again,\r\n");
    DBG_Puts("or flash the FOC app to use this calibration.\r\n");

    for (;;)
    {
        Board_DelayMs(1000u);
    }
}
