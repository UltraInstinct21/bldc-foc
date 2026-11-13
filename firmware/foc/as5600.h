/**
 * @file    as5600.h
 * @brief   Non-blocking AS5600 magnetic encoder driver (STM32 HAL, I2C).
 *
 * Wiring on the bldc_control_final_v6 board:
 *   SCL -> PA15  (J6 pin 2)
 *   SDA -> PB7   (J7 pin 2)      [costs USART1_RX]
 *   3V3 -> J6 pin 1
 *   GND -> J6 pin 3
 *
 * I2C1 on PA15/PB7 is AF4. External 4.7k pull-ups to 3V3 are required
 * (the board has none, most AS5600 breakouts have them fitted).
 */
#ifndef AS5600_H
#define AS5600_H

#include <stdint.h>
#include "stm32g4xx_hal.h"

#define AS5600_ADDR7        0x36u
#define AS5600_ADDR         (AS5600_ADDR7 << 1)

#define AS5600_REG_STATUS   0x0Bu
#define AS5600_REG_RAW      0x0Cu   /* RAW_ANGLE, use this for control (no filter lag) */
#define AS5600_REG_ANGLE    0x0Eu   /* filtered + scaled angle, display only */

/* STATUS bits */
#define AS5600_ST_MD        (1u << 5)   /* magnet detected      */
#define AS5600_ST_ML        (1u << 4)   /* magnet too weak      */
#define AS5600_ST_MH        (1u << 3)   /* magnet too strong    */

/* Blocking init: verifies the device answers and captures STATUS once. */
int      AS5600_Init(I2C_HandleTypeDef *hi2c, uint8_t *status_out);

/* Start a 2-byte RAW_ANGLE read. Non-blocking. Returns 0 if started,
 * -1 if a transfer is already in flight. Call at 2-5 kHz, not per 20 kHz ISR. */
int      AS5600_StartRead(void);

/* Call from HAL_I2C_MemRxCpltCallback(). */
void     AS5600_RxComplete(void);

/* Call from HAL_I2C_ErrorCallback(). */
void     AS5600_Error(void);

/* Latest 12-bit angle (0..4095) and a sequence number that increments per sample. */
uint16_t AS5600_GetRaw(void);
uint32_t AS5600_GetSequence(void);
uint8_t  AS5600_GetStatus(void);
uint32_t AS5600_GetErrorCount(void);

/* Blocking single read, used only during bring-up / calibration. */
int      AS5600_ReadBlocking(uint16_t *raw, uint32_t timeout_ms);

#endif /* AS5600_H */
