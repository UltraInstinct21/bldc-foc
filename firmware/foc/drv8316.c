/**
 * @file    drv8316.c
 * @brief   Minimal SPI setup + fault access for the DRV8316R.
 */
#include "drv8316.h"

#define DRV8316_SPI_TIMEOUT_MS  10u
#define DRV8316_READBACK_MASK   0x7Eu   /* ignore CLR_FLT (W1C) and bit 7 */

static SPI_HandleTypeDef *s_hspi;
static GPIO_TypeDef      *s_cs_port;
static uint16_t           s_cs_pin;

/* nSCS must stay high for >= 300 ns between frames. A volatile loop is
 * crude but keeps the driver independent of a timer/DWT. At 170 MHz the
 * loop below is ~400-600 ns including overhead. */
static void cs_high_gap(void)
{
    for (volatile uint32_t i = 0u; i < 20u; i++)
    {
        __NOP();
    }
}

/* Build the 16-bit SDI word with even parity in B8. */
static uint16_t make_frame(uint8_t addr, uint8_t data, uint8_t read)
{
    uint16_t w = (uint16_t)(((read != 0u) ? 0x8000u : 0u) |
                            ((uint16_t)(addr & 0x3Fu) << 9) |
                            (uint16_t)data);
    uint16_t x = (uint16_t)(w ^ (uint16_t)(w >> 8));

    x ^= (uint16_t)(x >> 4);
    x ^= (uint16_t)(x >> 2);
    x ^= (uint16_t)(x >> 1);

    if ((x & 1u) != 0u)
    {
        w |= 0x0100u;
    }

    return w;
}

static int xfer16(uint16_t tx, uint16_t *rx)
{
    uint16_t r = 0u;

    if (s_hspi == NULL)
    {
        return -1;
    }

    HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_RESET);

    if (HAL_SPI_TransmitReceive(s_hspi, (uint8_t *)&tx, (uint8_t *)&r, 1u,
                                DRV8316_SPI_TIMEOUT_MS) != HAL_OK)
    {
        HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_SET);
        cs_high_gap();
        return -1;
    }

    HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_SET);
    cs_high_gap();

    if (rx != NULL)
    {
        *rx = r;
    }

    return 0;
}

int DRV8316_Write(uint8_t addr, uint8_t data)
{
    return xfer16(make_frame(addr, data, 0u), NULL);
}

int DRV8316_Read(uint8_t addr, uint8_t *data)
{
    uint16_t rx;
    int      rc;

    if (data == NULL)
    {
        return -1;
    }

    rc = xfer16(make_frame(addr, 0x00u, 1u), &rx);
    if (rc != 0)
    {
        return rc;
    }

    /* B15:8 is status, B7:0 is the register data. */
    *data = (uint8_t)(rx & 0x00FFu);
    return 0;
}

int DRV8316_Init(SPI_HandleTypeDef *hspi, GPIO_TypeDef *cs_port,
                 uint16_t cs_pin)
{
    uint8_t v = 0u;
    int     rc;

    if ((hspi == NULL) || (cs_port == NULL))
    {
        return -1;
    }

    /* Fail early on a CubeMX setting mismatch instead of producing garbage
     * frames. Mode 1 = CPOL low, CPHA 1 edge, 16-bit, MSB first, soft NSS. */
    if ((hspi->Init.Mode          != SPI_MODE_MASTER)      ||
        (hspi->Init.DataSize      != SPI_DATASIZE_16BIT)   ||
        (hspi->Init.CLKPolarity   != SPI_POLARITY_LOW)     ||
        (hspi->Init.CLKPhase      != SPI_PHASE_1EDGE)      ||
        (hspi->Init.FirstBit      != SPI_FIRSTBIT_MSB)     ||
        (hspi->Init.NSS           != SPI_NSS_SOFT))
    {
        return -2;
    }

    s_hspi    = hspi;
    s_cs_port = cs_port;
    s_cs_pin  = cs_pin;

    HAL_GPIO_WritePin(s_cs_port, s_cs_pin, GPIO_PIN_SET);
    cs_high_gap();

    /* Unlock, configure, lock. */
    rc = DRV8316_Write(DRV8316_REG_CTRL_1, DRV8316_CTRL1_UNLOCK);
    if (rc != 0)
    {
        return -3;
    }

    rc = DRV8316_Write(DRV8316_REG_CTRL_2, DRV8316_CTRL2_6X_200V);
    if (rc != 0)
    {
        return -3;
    }

    rc = DRV8316_Write(DRV8316_REG_CTRL_5, DRV8316_CSA_015);
    if (rc != 0)
    {
        return -3;
    }

    /* Delay compensation is deliberately left at reset (off). It would make
     * the output delay fixed (TI suggests DLY_TARGET = 5 = 1.2 us at
     * 200 V/us) and removes duty-cycle distortion, but it also shifts the
     * low-side sampling window later. Enable it only if the current
     * waveform shows duty distortion:
     *   unlock; write CTRL_10 = 0x15; lock;
     */

    rc = DRV8316_Write(DRV8316_REG_CTRL_1, DRV8316_CTRL1_LOCK);
    if (rc != 0)
    {
        return -3;
    }

    /* Readback proves both directions of the SPI link and the new setting. */
    rc = DRV8316_Read(DRV8316_REG_CTRL_2, &v);
    if (rc != 0)
    {
        return -3;
    }

    if ((v & DRV8316_READBACK_MASK) !=
        (DRV8316_CTRL2_6X_200V & DRV8316_READBACK_MASK))
    {
        return -4;
    }

    return 0;
}

int DRV8316_ClearFault(void)
{
    uint8_t v = 0u;
    int     rc;

    rc = DRV8316_Read(DRV8316_REG_CTRL_2, &v);
    if (rc != 0)
    {
        return rc;
    }

    rc = DRV8316_Write(DRV8316_REG_CTRL_1, DRV8316_CTRL1_UNLOCK);
    if (rc != 0)
    {
        return rc;
    }

    rc = DRV8316_Write(DRV8316_REG_CTRL_2, (uint8_t)(v | 0x01u));

    (void)DRV8316_Write(DRV8316_REG_CTRL_1, DRV8316_CTRL1_LOCK);

    return rc;
}

int DRV8316_GetStatus(Drv8316Status *st)
{
    int rc;

    if (st == NULL)
    {
        return -1;
    }

    rc = DRV8316_Read(DRV8316_REG_IC_STATUS, &st->ic_status);
    if (rc != 0)
    {
        return rc;
    }

    rc = DRV8316_Read(DRV8316_REG_STATUS_1, &st->status1);
    if (rc != 0)
    {
        return rc;
    }

    return DRV8316_Read(DRV8316_REG_STATUS_2, &st->status2);
}
