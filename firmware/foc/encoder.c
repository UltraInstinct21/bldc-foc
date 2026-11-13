/**
 * @file    encoder.c
 * @brief   AS5600-based rotor angle and speed for sensored FOC.
 *
 * Time base: DWT cycle counter at the core clock (170 MHz by default).
 * Change ENCODER_CPU_HZ if you change the clock tree.
 */
#include "encoder.h"
#include "as5600.h"

#include <math.h>
#include <string.h>

#include "stm32g4xx_hal.h"

#define TWO_PI_F                (6.28318530718f)
#define PI_F                    (3.14159265359f)

#ifndef ENCODER_CPU_HZ
#define ENCODER_CPU_HZ          (170000000.0f)
#endif

#ifndef ENCODER_READ_DIV
#define ENCODER_READ_DIV        5u      /* 20 kHz loop -> 4 kHz encoder */
#endif

#define ENCODER_SPEED_ALPHA     0.25f
#define ENCODER_MAX_EXTRAP_S    0.005f  /* never extrapolate more than 5 ms */

/* ---- flash layout -------------------------------------------------------- */
#define CAL_MAGIC               0x43414C31u     /* "CAL1" */
#ifndef CAL_FLASH_ADDR
#define CAL_FLASH_ADDR          0x0801F800u     /* last 2 KB page of 128 KB */
#endif

typedef struct
{
    uint32_t magic;
    float    sign;
    float    offset;
    uint32_t pole_pairs;
    uint32_t chk;
} CalRec;                                       /* 20 bytes, padded to 24 on write */

/* ---- state --------------------------------------------------------------- */
static EncoderCal   s_cal;
static EncoderState s_st;

static float    s_prev_mech;        /* previous wrapped mechanical angle */
static uint32_t s_prev_tick;
static uint8_t  s_have_prev;

/* -------------------------------------------------------------------------- */
static float wrap_2pi(float a)
{
    float t;
    int32_t k;

    t = a * (1.0f / TWO_PI_F);
    k = (int32_t)t;                 /* truncation toward zero is fine here */
    a -= (float)k * TWO_PI_F;
    if (a < 0.0f)
    {
        a += TWO_PI_F;
    }
    return a;
}

static float wrap_pm_pi(float a)
{
    a = wrap_2pi(a);
    if (a > PI_F)
    {
        a -= TWO_PI_F;
    }
    return a;
}

static void dwt_init(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0u;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

/* -------------------------------------------------------------------------- */
void Encoder_Init(void)
{
    dwt_init();

    memset(&s_st, 0, sizeof(s_st));
    s_have_prev = 0u;

    if (Encoder_LoadCalibration() != 0)
    {
        s_cal.sign       = 1.0f;
        s_cal.offset     = 0.0f;
        s_cal.pole_pairs = (uint16_t)MOTOR_POLE_PAIRS;
        s_cal.valid      = 0u;
    }
}

/* Runs in interrupt context (I2C completion callback). Keep it short. */
void Encoder_OnSample(uint16_t raw12)
{
    float    mech;
    float    d;
    float    dt;
    float    w;
    uint32_t now;

    if (raw12 > 4095u)
    {
        s_st.bad_samples++;
        return;
    }

    mech = (float)raw12 * (TWO_PI_F / 4096.0f);
    now  = DWT->CYCCNT;

    if (s_have_prev == 0u)
    {
        s_prev_mech      = mech;
        s_prev_tick      = now;
        s_have_prev      = 1u;
        s_st.theta_mech  = mech;
        s_st.omega_mech  = 0.0f;
        s_st.tick        = now;
        s_st.valid       = 1u;
        return;
    }

    d  = mech - s_prev_mech;
    if (d > PI_F)       { d -= TWO_PI_F; }
    else if (d < -PI_F) { d += TWO_PI_F; }

    dt = (float)(uint32_t)(now - s_prev_tick) / ENCODER_CPU_HZ;

    s_st.theta_mech += d;

    if (dt > 1.0e-6f)
    {
        w = d / dt;
        s_st.omega_mech += ENCODER_SPEED_ALPHA * (w - s_st.omega_mech);
    }

    s_prev_mech = mech;
    s_prev_tick = now;

    s_st.tick = now;
    s_st.seq++;
    s_st.valid = 1u;
}

void Encoder_Tick(void)
{
    static uint8_t div = 0u;

    div++;
    if (div >= ENCODER_READ_DIV)
    {
        div = 0u;
        (void)AS5600_StartRead();
    }
}

float Encoder_ElectricalAngle(void)
{
    float    dt;
    float    th;
    float    e;
    uint32_t now;

    if (s_st.valid == 0u)
    {
        return 0.0f;
    }

    now = DWT->CYCCNT;
    dt  = (float)(uint32_t)(now - s_st.tick) / ENCODER_CPU_HZ;

    if (dt > ENCODER_MAX_EXTRAP_S)
    {
        dt = 0.0f;      /* encoder stalled: fall back to last known angle */
    }

    th = s_st.theta_mech + (s_st.omega_mech * dt);
    e  = (s_cal.sign * th * (float)s_cal.pole_pairs) - s_cal.offset;

    return wrap_2pi(e);
}

float Encoder_MechanicalSpeed(void)
{
    return s_st.omega_mech;
}

float Encoder_ElectricalSpeed(void)
{
    return s_st.omega_mech * (float)s_cal.pole_pairs;
}

const EncoderState *Encoder_GetState(void)
{
    return &s_st;
}

const EncoderCal *Encoder_GetCal(void)
{
    return &s_cal;
}

/* -------------------------------------------------------------------------- */
/* Calibration                                                                 */
/* -------------------------------------------------------------------------- */

/* Average 8 blocking samples into a wrapped mechanical angle. */
static int read_mech_blocking(float *mech)
{
    float    sum_s = 0.0f;
    float    sum_c = 0.0f;
    uint16_t raw;
    int      n = 0;

    for (uint32_t i = 0u; i < 8u; i++)
    {
        if (AS5600_ReadBlocking(&raw, 20u) == 0)
        {
            float a = (float)raw * (TWO_PI_F / 4096.0f);
            sum_s += sinf(a);
            sum_c += cosf(a);
            n++;
        }
        HAL_Delay(2u);
    }

    if (n == 0)
    {
        return -1;
    }

    *mech = atan2f(sum_s, sum_c);
    if (*mech < 0.0f)
    {
        *mech += TWO_PI_F;
    }
    return 0;
}

int Encoder_Calibrate(void (*apply_vector)(float theta_e, float modulation),
                      uint32_t hold_ms, float mod)
{
    static const float cmd[3] =
    {
        0.0f,
        TWO_PI_F / 3.0f,
        2.0f * TWO_PI_F / 3.0f
    };

    float    m0, m1, d;
    float    sum_s = 0.0f;
    float    sum_c = 0.0f;
    float    mech;

    if (apply_vector == NULL)
    {
        return -1;
    }
    if (hold_ms < 50u)
    {
        hold_ms = 50u;
    }

    /* --- 1. direction probe ------------------------------------------------ */
    apply_vector(0.0f, mod);
    HAL_Delay(hold_ms);
    if (read_mech_blocking(&m0) != 0)
    {
        return -2;
    }

    apply_vector(PI_F * 0.5f, mod);
    HAL_Delay(hold_ms);
    if (read_mech_blocking(&m1) != 0)
    {
        return -2;
    }

    d = wrap_pm_pi(m1 - m0);
    if (fabsf(d) < 0.02f)
    {
        return -3;      /* rotor did not follow: too little torque / not aligned */
    }
    s_cal.sign = (d > 0.0f) ? 1.0f : -1.0f;

    /* --- 2. offset at three electrical angles ----------------------------- */
    for (uint32_t i = 0u; i < 3u; i++)
    {
        float off;

        apply_vector(cmd[i], mod);
        HAL_Delay(hold_ms);

        if (read_mech_blocking(&mech) != 0)
        {
            return -2;
        }

        off = wrap_pm_pi((s_cal.sign * mech * (float)MOTOR_POLE_PAIRS) - cmd[i]);
        sum_s += sinf(off);
        sum_c += cosf(off);
    }

    s_cal.offset     = atan2f(sum_s, sum_c);
    s_cal.pole_pairs = (uint16_t)MOTOR_POLE_PAIRS;
    s_cal.valid      = 1u;

    /* --- 3. release the bridge and persist -------------------------------- */
    apply_vector(0.0f, 0.0f);

    {
        int rc = Encoder_SaveCalibration();

        /* Restart angle tracking from the next sample so the tracker cannot
         * carry a stale multi-turn offset from before calibration. */
        s_have_prev  = 0u;
        s_st.valid   = 0u;

        return rc;
    }
}

/* -------------------------------------------------------------------------- */
/* Flash persistence                                                           */
/* -------------------------------------------------------------------------- */

static uint32_t cal_checksum(const CalRec *r)
{
    const uint8_t *p = (const uint8_t *)r;
    uint32_t       s = 0x12345678u;

    for (uint32_t i = 0u; i < 16u; i++)     /* magic..pole_pairs */
    {
        s = (s * 33u) ^ (uint32_t)p[i];
    }
    return s;
}

int Encoder_LoadCalibration(void)
{
    CalRec rec;

    memcpy(&rec, (const void *)CAL_FLASH_ADDR, sizeof(rec));

    if ((rec.magic != CAL_MAGIC) ||
        (rec.chk != cal_checksum(&rec)) ||
        (rec.pole_pairs != (uint32_t)MOTOR_POLE_PAIRS) ||
        (fabsf(rec.sign) < 0.5f))
    {
        s_cal.valid = 0u;
        return -1;
    }

    s_cal.sign       = rec.sign;
    s_cal.offset     = rec.offset;
    s_cal.pole_pairs = (uint16_t)rec.pole_pairs;
    s_cal.valid      = 1u;

    return 0;
}

int Encoder_SaveCalibration(void)
{
    CalRec             rec;
    uint64_t           words[3];
    FLASH_EraseInitTypeDef er = {0};
    uint32_t           page_err = 0u;

    rec.magic      = CAL_MAGIC;
    rec.sign       = s_cal.sign;
    rec.offset     = s_cal.offset;
    rec.pole_pairs = (uint32_t)s_cal.pole_pairs;
    rec.chk        = cal_checksum(&rec);

    memset(words, 0, sizeof(words));
    memcpy(words, &rec, sizeof(rec));

    HAL_FLASH_Unlock();
    __HAL_FLASH_CLEAR_FLAG(FLASH_FLAG_ALL_ERRORS);

    er.TypeErase = FLASH_TYPEERASE_PAGES;
    er.Banks     = FLASH_BANK_1;
    er.Page      = (uint32_t)((CAL_FLASH_ADDR - FLASH_BASE) / FLASH_PAGE_SIZE);
    er.NbPages   = 1u;

    if (HAL_FLASHEx_Erase(&er, &page_err) != HAL_OK)
    {
        HAL_FLASH_Lock();
        return -1;
    }

    for (uint32_t i = 0u; i < 3u; i++)
    {
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                              CAL_FLASH_ADDR + (i * 8u),
                              words[i]) != HAL_OK)
        {
            HAL_FLASH_Lock();
            return -2;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}

void Encoder_ResetCalibration(void)
{
    s_cal.sign       = 1.0f;
    s_cal.offset     = 0.0f;
    s_cal.pole_pairs = (uint16_t)MOTOR_POLE_PAIRS;
    s_cal.valid      = 0u;
}
