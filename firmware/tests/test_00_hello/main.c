/**
 * @file    main.c
 * @brief   test_00_hello — power, clock, UART and GPIO smoke test.
 *
 * Covers the always-on part of the board:
 *   - 3V3 regulator and MCU boot
 *   - 170 MHz clock tree (HSI16 -> PLL)
 *   - USART1 TX on PB6 / J7.1 (115200 8N1)
 *   - nSLEEP / DRVOFF / nFAULT pin states (hardware strapped DRV8316CT)
 *
 * Expected UART output:
 *   banner, clock frequencies, UID, then a 1 Hz status line.
 *
 * No motor output is enabled. nSLEEP stays low and DRVOFF stays high.
 */
#include "board.h"
#include "debug.h"

int main(void)
{
    Board_Init();

    DBG_Puts("\r\n\r\n");
    DBG_Puts("==================================================\r\n");
    DBG_Puts(" bldc_control_final_v6  |  test_00_hello\r\n");
    DBG_Puts(" STM32G431CBT6 + DRV8316CT (Hardware Mode)\r\n");
    DBG_Puts("==================================================\r\n");

    DBG_Printf("build      : %s %s\r\n", __DATE__, __TIME__);
    DBG_Printf("SYSCLK     : %lu Hz\r\n", (unsigned long)HAL_RCC_GetSysClockFreq());
    DBG_Printf("HCLK       : %lu Hz\r\n", (unsigned long)HAL_RCC_GetHCLKFreq());
    DBG_Printf("PCLK1      : %lu Hz\r\n", (unsigned long)HAL_RCC_GetPCLK1Freq());
    DBG_Printf("PCLK2      : %lu Hz\r\n", (unsigned long)HAL_RCC_GetPCLK2Freq());
    DBG_Printf("UID        : %08lX-%08lX-%08lX\r\n",
               (unsigned long)HAL_GetUIDw0(),
               (unsigned long)HAL_GetUIDw1(),
               (unsigned long)HAL_GetUIDw2());

    DBG_Puts("\r\nControl pins:\r\n");
    DBG_Puts("  nSLEEP = PC13 [expected 0 = DRV asleep]\r\n");
    DBG_Puts("  DRVOFF = PA7  [expected 1 = outputs off]\r\n");
    DBG_Puts("  nFAULT = PA6  [expected 1 = no fault]\r\n");

    DBG_Puts("\r\nIf you can read this, power, clock and UART are good.\r\n");
    DBG_Puts("Status line follows once per second.\r\n\r\n");

    for (;;)
    {
        DBG_Printf("t=%6lu ms  nSLEEP=%u  DRVOFF=%u  nFAULT=%u\r\n",
                   (unsigned long)Board_Millis(),
                   (unsigned)HAL_GPIO_ReadPin(DRV_NSLEEP_PORT, DRV_NSLEEP_PIN),
                   (unsigned)HAL_GPIO_ReadPin(DRV_DRVOFF_PORT, DRV_DRVOFF_PIN),
                   (unsigned)HAL_GPIO_ReadPin(DRV_NFAULT_PORT, DRV_NFAULT_PIN));
        Board_DelayMs(1000u);
    }
}
