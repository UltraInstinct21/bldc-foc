/**
 * @file    main.c
 * @brief   test_02_drv8316_spi — SPI1 gate-driver configuration test.
 *
 * Covers:
 *   - SPI1 on PB3/PB4/PB5 (AF5) with software chip select PA4
 *   - DRV8316R 16-bit frame format, even parity, mode 1
 *   - register write / read-back (6x PWM, 200 V/us slew, CSA 0.15 V/A)
 *   - status register decoding and nFAULT
 *
 * The bridge is woken up for the test but DRVOFF is left HIGH, so the
 * power stage stays Hi-Z. No motor current flows.
 *
 * Expected: "DRV8316_Init rc=0" and CTRL_2 low byte 0x78.
 */
#include "board.h"
#include "debug.h"
#include "drv8316.h"

static void print_status(void)
{
    Drv8316Status st = {0};

    if (DRV8316_GetStatus(&st) != 0)
    {
        DBG_Puts("  status read failed (SPI link problem)\r\n");
        return;
    }

    DBG_Printf("  IC_STATUS=0x%02X  STATUS_1=0x%02X  STATUS_2=0x%02X\r\n",
               st.ic_status, st.status1, st.status2);

    DBG_Printf("  fault=%u ot=%u ovp=%u npor=%u ocp=%u spi_flt=%u bk_flt=%u\r\n",
               (unsigned)((st.ic_status >> 0) & 1u),
               (unsigned)((st.ic_status >> 1) & 1u),
               (unsigned)((st.ic_status >> 2) & 1u),
               (unsigned)((st.ic_status >> 3) & 1u),
               (unsigned)((st.ic_status >> 4) & 1u),
               (unsigned)((st.ic_status >> 5) & 1u),
               (unsigned)((st.ic_status >> 6) & 1u));
}

int main(void)
{
    uint8_t v = 0u;
    int     rc;

    Board_Init();
    Board_InitSPI1();

    DBG_Puts("\r\n\r\n=== test_02_drv8316_spi (SPI1 PB3/4/5, CS PA4) ===\r\n");

    /* nSLEEP low -> high. SPI registers are reset on wake. */
    Board_DrvSleep();
    Board_DelayMs(2u);
    Board_DrvWake();
    Board_DelayMs(5u);

    DBG_Puts("Configuring DRV8316R over SPI...\r\n");
    rc = DRV8316_Init(&hspi1, DRV_NSCS_PORT, DRV_NSCS_PIN);
    DBG_Printf("DRV8316_Init rc=%d\r\n", rc);

    if (rc != 0)
    {
        DBG_Puts("  -1 bad handle, -2 CubeMX SPI settings wrong,\r\n");
        DBG_Puts("  -3 transfer failed, -4 readback mismatch.\r\n");
        DBG_Puts("  Check SPI mode 1, 16-bit, MSB first, PA4 GPIO CS,\r\n");
        DBG_Puts("  PB3/PB4/PB5 AF5, and 3V3 on the DRV.\r\n");
    }
    else
    {
        DBG_Puts("SPI link OK.\r\n");
    }

    if (DRV8316_Read(DRV8316_REG_CTRL_2, &v) == 0)
    {
        DBG_Printf("CTRL_2 (0x04) = 0x%02X  [expect 0x78: 6x, 200 V/us]\r\n", v);
    }
    if (DRV8316_Read(DRV8316_REG_CTRL_5, &v) == 0)
    {
        DBG_Printf("CTRL_5 (0x07) = 0x%02X  [expect 0x00: CSA 0.15 V/A]\r\n", v);
    }

    print_status();

    DBG_Puts("\r\nPolling status every 500 ms. DRVOFF stays HIGH (outputs off).\r\n\r\n");

    for (;;)
    {
        if (Board_DrvFault() != 0u)
        {
            DBG_Puts("nFAULT asserted -> clearing...\r\n");
            (void)DRV8316_ClearFault();
            Board_DelayMs(10u);
        }

        print_status();
        Board_DelayMs(500u);
    }
}
