/**
 * @file    main.c
 * @brief   test_01_as5600 — I2C1 encoder test.
 *
 * Covers:
 *   - I2C1 on PA15 (SCL, J6.2) / PB7 (SDA, J7.2), 400 kHz
 *   - AS5600 presence, STATUS (magnet detected / too weak / too strong)
 *   - RAW_ANGLE tracking and a simple speed estimate
 *
 * Wiring (AS5600 breakout):
 *   SCL -> J6.2, SDA -> J7.2, VCC -> J6.1 (3V3), GND -> J6.3
 *   Pull-ups: 4.7k to 3V3 on both SCL and SDA (breakout usually has them).
 *
 * Turn the shaft by hand: RAW must sweep 0..4095 exactly once per turn.
 */
#include "board.h"
#include "debug.h"
#include "as5600.h"

static int32_t wrap_delta(uint16_t prev, uint16_t now)
{
    int32_t d = (int32_t)now - (int32_t)prev;

    if (d > 2048)
    {
        d -= 4096;
    }
    else if (d < -2048)
    {
        d += 4096;
    }
    return d;
}

int main(void)
{
    uint8_t status = 0u;
    uint16_t prev = 0u;
    uint32_t t_prev;
    float rpm = 0.0f;
    uint32_t n = 0u;

    Board_Init();
    Board_InitI2C1();

    DBG_Puts("\r\n\r\n=== test_01_as5600 (I2C1 PA15/PB7) ===\r\n");

    if (AS5600_Init(&hi2c1, &status) != 0)
    {
        DBG_Puts("AS5600 did not answer at 0x36.\r\n");
        DBG_Puts("Check: 3V3 on J6.1, GND on J6.3, SCL=J6.2, SDA=J7.2,\r\n");
        DBG_Puts("       4.7k pull-ups, and that PB7 is not driven by UART RX.\r\n");
        for (;;)
        {
            Board_DelayMs(1000u);
        }
    }

    DBG_Printf("AS5600 found. STATUS=0x%02X  MD=%u ML=%u MH=%u\r\n",
               status,
               (unsigned)((status & AS5600_ST_MD) ? 1u : 0u),
               (unsigned)((status & AS5600_ST_ML) ? 1u : 0u),
               (unsigned)((status & AS5600_ST_MH) ? 1u : 0u));

    if ((status & AS5600_ST_MD) == 0u)
    {
        DBG_Puts("WARNING: no magnet detected. Place the diametric magnet\r\n");
        DBG_Puts("         over the AS5600 at the recommended air gap.\r\n");
    }

    if (AS5600_ReadBlocking(&prev, 20u) != 0)
    {
        DBG_Puts("First RAW_ANGLE read failed.\r\n");
    }
    t_prev = Board_Millis();

    DBG_Puts("\r\n  raw   angle_deg   rpm\r\n");

    for (;;)
    {
        uint16_t raw = 0u;
        uint32_t t;

        if (AS5600_ReadBlocking(&raw, 20u) == 0)
        {
            int32_t  d = wrap_delta(prev, raw);
            uint32_t dt;

            t  = Board_Millis();
            dt = (t - t_prev) ? (t - t_prev) : 1u;

            /* rpm = delta_counts * (360/4096) deg / dt_sec / 360 * 60 */
            rpm = (float)d * (60.0f / 4096.0f) * (1000.0f / (float)dt);

            prev   = raw;
            t_prev = t;

            if ((n % 5u) == 0u)
            {
                DBG_Printf("%5u   %8.2f   %8.1f\r\n",
                           (unsigned)raw,
                           (double)((float)raw * 360.0f / 4096.0f),
                           (double)rpm);
            }
            n++;
        }

        Board_DelayMs(20u);
    }
}
