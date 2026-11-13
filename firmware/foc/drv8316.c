/**
 * @file    drv8316.c
 * @brief   DRV8316CT hardware-variant helper.
 */
#include "drv8316.h"
#include "board.h"

/* The datasheet asks for a 20 to 40 us low pulse on nSLEEP to clear a latched
 * fault without entering sleep. At 170 MHz this loop is roughly 30 us. */
static void fault_reset_pulse(void)
{
    Board_DrvSleep();

    for (volatile uint32_t i = 0u; i < 1300u; i++)
    {
        __NOP();
    }

    Board_DrvWake();
    Board_DelayMs(2u);              /* tWAKE */
}

int DRV8316_ClearFault(void)
{
    fault_reset_pulse();

    return (Board_DrvFault() != 0u) ? -1 : 0;
}

int DRV8316_Init(void)
{
    Board_DelayMs(2u);              /* nSLEEP must be high for tWAKE */

    if (Board_DrvFault() == 0u)
    {
        return 0;
    }

    /* One retry: the first fault may be a power-up UVLO that has cleared. */
    return DRV8316_ClearFault();
}

uint8_t DRV8316_Fault(void)
{
    return Board_DrvFault();
}
