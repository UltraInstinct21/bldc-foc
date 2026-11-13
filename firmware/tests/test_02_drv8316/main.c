/**
 * @file    main.c
 * @brief   test_02_drv8316 — DRV8316CT hardware-mode driver & fault test.
 *
 * Covers:
 *   - DRV8316CT hardware mode (no SPI)
 *   - Wake sequence via nSLEEP (PC13)
 *   - DRVOFF (PA7) held HIGH so power stage stays Hi-Z (no motor current)
 *   - nFAULT (PA6) monitoring and latched fault clearing via nSLEEP pulse
 *   - Verification of hardware strap defaults:
 *       MODE = AGND (6x PWM), GAIN = Hi-Z (0.3 V/A), SLEW = 47k (125 V/us)
 *
 * Expected output:
 *   "DRV8316_Init rc=0"
 *   Followed by periodic "nFAULT=0 (OK)" lines every 500 ms.
 */
#include "board.h"
#include "debug.h"

int main(void)
{
    int rc;

    Board_Init();

    DBG_Puts("\r\n\r\n=== test_02_drv8316 (DRV8316CT Hardware Mode) ===\r\n");
    DBG_Puts("Hardware straps: MODE=6x PWM, GAIN=0.3 V/A, SLEW=125 V/us, OCP=16 A\r\n");
    DBG_Puts("DRVOFF is HIGH: motor outputs remain Hi-Z.\r\n\r\n");

    /* Wake the gate driver: nSLEEP low -> high. */
    DBG_Puts("Waking DRV8316CT (nSLEEP low -> high)...\r\n");
    Board_DrvSleep();
    Board_DelayMs(2u);
    Board_DrvWake();
    Board_DelayMs(5u);

    rc = DRV8316_Init();
    DBG_Printf("DRV8316_Init rc=%d\r\n", rc);

    if (rc != 0)
    {
        DBG_Puts("  nFAULT is ASSERTED after wake and clear attempt.\r\n");
        DBG_Puts("  Check VM supply voltage (4.5 to 35 V), GND connection,\r\n");
        DBG_Puts("  and ensure no short circuit exists on phase outputs.\r\n");
    }
    else
    {
        DBG_Puts("DRV8316CT power-up OK, nFAULT de-asserted.\r\n");
    }

    DBG_Puts("\r\nMonitoring nFAULT every 500 ms...\r\n\r\n");

    for (;;)
    {
        uint8_t fault = Board_DrvFault();

        if (fault != 0u)
        {
            DBG_Puts("nFAULT asserted! Attempting fault clear via nSLEEP pulse...\r\n");
            int clr_rc = DRV8316_ClearFault();
            DBG_Printf("  ClearFault rc=%d, nFAULT=%u\r\n", clr_rc, (unsigned)Board_DrvFault());
        }
        else
        {
            DBG_Printf("t=%6lu ms  nFAULT=0 (OK)  DRVOFF=1 (safe)\r\n",
                       (unsigned long)Board_Millis());
        }

        Board_DelayMs(500u);
    }
}
