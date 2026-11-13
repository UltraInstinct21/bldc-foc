/**
 * @file    drv8316.h
 * @brief   Minimal SPI configuration + fault access for the DRV8316R.
 *
 * The DRV8316R powers up in 6x PWM mode with a 25 V/us slew rate. At 24 V
 * that raw default is too slow for 3-shunt current sampling at 20 kHz and
 * 0.84 duty (the internal dead time is 1.8-3.4 us). This driver changes:
 *
 *   - SLEW = 200 V/us   (dead time 0.5-0.75 us)
 *   - PWM_MODE = 6x     (TIM1 CHx/CHxN drive INHx/INLx)
 *   - CSA_GAIN = 0.15 V/A (VREF/2 = 1.65 V offset, +/-8 A linear range)
 *
 * Frame: 16 bits, MSB first, SPI mode 1 (CPOL = 0, CPHA = 1), <= 10 MHz.
 *   B15    = W (0 = write, 1 = read)
 *   B14:9  = 6-bit register address
 *   B8     = even parity over the other 15 bits
 *   B7:0   = data
 * SDO: B15:8 = status, B7:0 = register data (for a read).
 *
 * Wiring on bldc_control_final_v6:
 *   SCLK = PB3 (AF6), MISO = PB4 (AF6), MOSI = PB5 (AF6), nSCS = PA4 (GPIO).
 *
 * Call DRV8316_Init() after nSLEEP has been high for >= 1 ms (tWAKE) and
 * before enabling the bridge. PA4 must be a GPIO output, not SPI1_NSS AF.
 *
 * NOTE: entering sleep resets all SPI registers, including SLEW. After any
 * nSLEEP low pulse, call DRV8316_Init() again. Clear faults with
 * DRV8316_ClearFault() instead of cycling nSLEEP.
 */
#ifndef DRV8316_H
#define DRV8316_H

#include <stdint.h>
#include "stm32g4xx_hal.h"

/* ---- register map (only what this driver needs) ------------------------- */
#define DRV8316_REG_IC_STATUS   0x00u
#define DRV8316_REG_STATUS_1    0x01u
#define DRV8316_REG_STATUS_2    0x02u
#define DRV8316_REG_CTRL_1      0x03u
#define DRV8316_REG_CTRL_2      0x04u
#define DRV8316_REG_CTRL_5      0x07u
#define DRV8316_REG_CTRL_10     0x0Cu

/* CONTROL_1: write 011b to unlock, 110b to lock. */
#define DRV8316_CTRL1_UNLOCK    0x03u
#define DRV8316_CTRL1_LOCK      0x06u

/* CONTROL_2: reserved 01b, SDO push-pull, SLEW = 11b (200 V/us),
 * PWM_MODE = 00b (6x), CLR_FLT = 0. */
#define DRV8316_CTRL2_6X_200V   0x78u

/* CONTROL_5: CSA gain in bits 1:0. */
#define DRV8316_CSA_015         0x00u   /* 0.15 V/A, +/-8 A */
#define DRV8316_CSA_030         0x01u   /* 0.30 V/A, +/-4 A */
#define DRV8316_CSA_060         0x02u   /* 0.60 V/A, +/-2 A */
#define DRV8316_CSA_120         0x03u   /* 1.20 V/A, +/-1 A */

/* CONTROL_10: delay compensation. Optional; see the .c file. */
#define DRV8316_CTRL10_DLYCMP   0x10u

typedef struct
{
    uint8_t ic_status;      /* 0x00: BK_FLT SPI_FLT OCP NPOR OVP OT FAULT */
    uint8_t status1;        /* 0x01: OTW OTS OCP_HC OCL_LC ... OCP_LA */
    uint8_t status2;        /* 0x02: OTP_ERR BUCK_OCP BUCK_UV VCP_UV SPI_* */
} Drv8316Status;

/**
 * Configure the driver and verify the SPI link.
 *
 * @param hspi      SPI1 handle, 16-bit, mode 1, MSB first, software NSS.
 * @param cs_port   GPIO port of nSCS (GPIOA).
 * @param cs_pin    GPIO pin  of nSCS (GPIO_PIN_4).
 * @return 0 on success, negative on a bad handle / SPI failure / readback
 *         mismatch. On failure the bridge is still in its power-up state.
 */
int DRV8316_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port,
                 uint16_t cs_pin);

/* Raw register access. Registers are locked after Init; unlock first if you
 * need to change a setting (write CTRL_1 = DRV8316_CTRL1_UNLOCK, ...). */
int DRV8316_Write(uint8_t addr, uint8_t data);
int DRV8316_Read(uint8_t addr, uint8_t *data);

/* Clear latched faults (CLR_FLT in CONTROL_2). Unlocks/locks around the
 * write because the DRV ignores CONTROL writes while locked. */
int DRV8316_ClearFault(void);

/* Read the three status registers in one go. */
int DRV8316_GetStatus(Drv8316Status *st);

#endif /* DRV8316_H */
