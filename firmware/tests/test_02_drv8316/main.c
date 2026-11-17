/**
 * @file    main.c
 * @brief   test_02_drv8316 — DRV8316T hardware-mode driver & fault test.
 *
 * Covers (same sequence as the bring-up "diag" command that was verified
 * on the board):
 *   - DRV8316T hardware mode (no SPI)
 *   - idle state: DRVOFF LOW + all six INx low (bridge Hi-Z). DRVOFF high is
 *     NOT an idle state: it makes the DRV pull nFAULT low (datasheet 8.4.2)
 *   - wake via nSLEEP (PC13), nFAULT low for ~1.8 ms after wake, then high
 *   - 30 us nSLEEP reset pulse (tRST 20-40 us)
 *   - DRVOFF high for 50 ms -> nFAULT low; DRVOFF low -> nFAULT high again.
 *     This proves the driver is powered and answering.
 *
 * Needs VIN on J1 (the DRV is powered from VM only). With VM missing,
 * nFAULT is clamped low by the chip and ignores nSLEEP.
 *
 * Expected output:
 *   "DRV8316_Init rc=0", "DRVOFF check: PASS",
 *   then "nFAULT=1 (OK)" every 500 ms.
 */
#include "board.h"
#include "debug.h"

/* After a wake, nFAULT goes low a little later and stays low ~1.8 ms
 * (bring-up measurement). Wait up to 5 ms for the falling edge, then time
 * the low phase with the DWT cycle counter. Returns microseconds, 0 if
 * nFAULT never went low. */
static uint32_t nfault_wake_pulse_us(void)
{
    const uint32_t cyc_per_us = SystemCoreClock / 1000000u;
    uint32_t t0 = DWT->CYCCNT;
    uint32_t t_fall;

    while (Board_DrvFault() == 0u)
    {
        if ((DWT->CYCCNT - t0) > 5000u * cyc_per_us)
        {
            return 0u;
        }
    }
    t_fall = DWT->CYCCNT;
    while ((Board_DrvFault() != 0u) && ((DWT->CYCCNT - t_fall) < 300000u * cyc_per_us))
    {
    }
    return (DWT->CYCCNT - t_fall) / cyc_per_us;
}

int main(void)
{
    int      rc;
    uint32_t low_us;
    uint8_t  f_drvoff_hi;
    uint8_t  f_drvoff_lo;

    Board_Init();

    DBG_Puts("\r\n\r\n=== test_02_drv8316 (DRV8316T Hardware Mode) ===\r\n");
    DBG_Puts("Straps: MODE=6x PWM, SLEW=125 V/us, GAIN=0.3 V/A, OCP/SR open (ASR/AAR on)\r\n");
    DBG_Puts("Idle: DRVOFF LOW, all INx low -> outputs Hi-Z.\r\n\r\n");

    /* Wake the gate driver: nSLEEP low -> high. */
    DBG_Puts("Waking DRV8316T (nSLEEP low -> high)...\r\n");
    Board_DrvSleep();
    Board_DelayMs(2u);
    Board_DrvWake();
    low_us = nfault_wake_pulse_us();
    DBG_Printf("  nFAULT low for %lu us after wake (bring-up measured ~1800 us)\r\n",
               (unsigned long)low_us);

    rc = DRV8316_Init();
    DBG_Printf("DRV8316_Init rc=%d\r\n", rc);

    if (rc != 0)
    {
        DBG_Puts("  nFAULT is still low after wake and a 30 us reset pulse.\r\n");
        DBG_Puts("  Most likely VM is missing: check VIN on J1, TP2/TP3, F1, Q1.\r\n");
        DBG_Puts("  Also make sure nothing holds DRVOFF high.\r\n");
    }
    else
    {
        DBG_Puts("DRV8316T awake, nFAULT high.\r\n");
    }

    /* DRVOFF check: the DRV should pull nFAULT low while DRVOFF is high. */
    Board_DrvOutputDisable();
    Board_DelayMs(50u);
    f_drvoff_hi = Board_DrvFault();
    Board_DrvOutputEnable();            /* back to the idle level */
    Board_DelayMs(20u);
    f_drvoff_lo = Board_DrvFault();

    DBG_Printf("DRVOFF high: nFAULT=%u (expect 0)   DRVOFF low: nFAULT=%u (expect 1)\r\n",
               (unsigned)(f_drvoff_hi ? 0u : 1u), (unsigned)(f_drvoff_lo ? 0u : 1u));
    DBG_Printf("DRVOFF check: %s\r\n",
               ((f_drvoff_hi != 0u) && (f_drvoff_lo == 0u)) ? "PASS" : "FAIL");

    DBG_Puts("\r\nMonitoring nFAULT every 500 ms (1 = OK)...\r\n\r\n");

    for (;;)
    {
        if (Board_DrvFaultDebounced() != 0u)
        {
            DBG_Puts("nFAULT asserted! Clearing with a 30 us nSLEEP pulse...\r\n");
            int clr_rc = DRV8316_ClearFault();
            DBG_Printf("  ClearFault rc=%d, nFAULT=%u\r\n", clr_rc,
                       (unsigned)(Board_DrvFault() ? 0u : 1u));
        }
        else
        {
            DBG_Printf("t=%6lu ms  nFAULT=1 (OK)  DRVOFF=0  INx=0 (Hi-Z)\r\n",
                       (unsigned long)Board_Millis());
        }

        Board_DelayMs(500u);
    }
}
