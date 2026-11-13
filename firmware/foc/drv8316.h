/**
 * @file    drv8316.h
 * @brief   DRV8316CT hardware-variant helper.
 *
 * The board revision v6 wires the driver in hardware mode:
 *
 *   MODE    = AGND        6x PWM, SOx active
 *   GAIN    = Hi-Z        0.3 V/A
 *   SLEW    = 47k to AVDD 125 V/us
 *   OCP/SR  = Hi-Z        16 A OCP, active demagnetization on
 *   VREF    = 3V3         1.65 V offset, about +/-4.6 A linear range
 *
 * There is no SPI connection. The firmware only controls nSLEEP and DRVOFF
 * and reads nFAULT, so this module is a thin wrapper around those pins.
 */
#ifndef DRV8316_H
#define DRV8316_H

#include <stdint.h>

#define DRV8316_CSA_V_PER_A     0.3f

/**
 * Check the driver after nSLEEP has been high for at least 1 ms.
 * Clears one latched fault by pulsing nSLEEP if needed.
 *
 * @return 0 if the driver reports no fault, -1 if nFAULT is still low.
 */
int DRV8316_Init(void);

/**
 * Reset latched faults with a short nSLEEP pulse (20 to 40 us).
 * The hardware straps are not affected.
 *
 * @return 0 on success, -1 if nFAULT is still low afterwards.
 */
int DRV8316_ClearFault(void);

/* 1 if nFAULT is asserted. */
uint8_t DRV8316_Fault(void);

#endif /* DRV8316_H */
