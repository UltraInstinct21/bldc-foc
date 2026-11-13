/**
 * @file    as5600.c
 * @brief   Non-blocking AS5600 driver.
 *
 * The RAW_ANGLE register (0x0C) is used instead of ANGLE (0x0E):
 * RAW_ANGLE is the unfiltered CORDIC output, so it does not carry the
 * 0.29-2.2 ms slow-filter delay. Filter in software at the control rate.
 */
#include "as5600.h"
#include <string.h>

#define AS5600_I2C_TIMEOUT_MS   20u

static I2C_HandleTypeDef *s_i2c;
static uint8_t  s_buf[2];
static volatile uint16_t s_raw;
static volatile uint32_t s_seq;
static volatile uint32_t s_err;
static volatile uint8_t  s_busy;
static uint8_t  s_status;

int AS5600_Init(I2C_HandleTypeDef *hi2c, uint8_t *status_out)
{
    uint8_t st = 0u;

    s_i2c     = hi2c;
    s_busy    = 0u;
    s_err     = 0u;
    s_seq     = 0u;
    s_raw     = 0u;
    s_status  = 0u;

    if (s_i2c == NULL)
    {
        return -1;
    }

    for (uint32_t i = 0u; i < 5u; i++)
    {
        if (HAL_I2C_Mem_Read(s_i2c, AS5600_ADDR, AS5600_REG_STATUS,
                             I2C_MEMADD_SIZE_8BIT, &st, 1u,
                             AS5600_I2C_TIMEOUT_MS) == HAL_OK)
        {
            s_status = st;
            if (status_out != NULL)
            {
                *status_out = st;
            }
            return 0;
        }
        HAL_Delay(5u);
    }

    return -2;
}

int AS5600_StartRead(void)
{
    if ((s_i2c == NULL) || (s_busy != 0u))
    {
        return -1;
    }

    s_busy = 1u;

    if (HAL_I2C_Mem_Read_IT(s_i2c, AS5600_ADDR, AS5600_REG_RAW,
                            I2C_MEMADD_SIZE_8BIT, s_buf, 2u) != HAL_OK)
    {
        s_busy = 0u;
        s_err++;
        return -2;
    }

    return 0;
}

void AS5600_RxComplete(void)
{
    s_raw = (uint16_t)(((uint16_t)(s_buf[0] & 0x0Fu) << 8) | (uint16_t)s_buf[1]);
    s_seq++;
    s_busy = 0u;
}

void AS5600_Error(void)
{
    s_err++;
    s_busy = 0u;
}

uint16_t AS5600_GetRaw(void)
{
    return s_raw;
}

uint32_t AS5600_GetSequence(void)
{
    return s_seq;
}

uint8_t AS5600_GetStatus(void)
{
    return s_status;
}

uint32_t AS5600_GetErrorCount(void)
{
    return s_err;
}

int AS5600_ReadBlocking(uint16_t *raw, uint32_t timeout_ms)
{
    uint8_t b[2];
    uint32_t t0;

    if ((s_i2c == NULL) || (raw == NULL))
    {
        return -1;
    }

    /* Abort any in-flight interrupt transfer first. */
    if (s_busy != 0u)
    {
        (void)HAL_I2C_Master_Abort_IT(s_i2c, AS5600_ADDR);
        s_busy = 0u;
    }

    t0 = HAL_GetTick();
    do
    {
        if (HAL_I2C_Mem_Read(s_i2c, AS5600_ADDR, AS5600_REG_RAW,
                             I2C_MEMADD_SIZE_8BIT, b, 2u,
                             AS5600_I2C_TIMEOUT_MS) == HAL_OK)
        {
            *raw = (uint16_t)(((uint16_t)(b[0] & 0x0Fu) << 8) | (uint16_t)b[1]);
            return 0;
        }
    } while ((HAL_GetTick() - t0) < timeout_ms);

    s_err++;
    return -2;
}
