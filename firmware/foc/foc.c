/**
 * @file    foc.c
 * @brief   Minimal sensored FOC for the DRV8316 board.
 *
 * Signal chain per 20 kHz cycle:
 *
 *   ADC (3 low-side shunts)
 *     -> ia/ib/ic -> Clarke -> Park(theta from encoder)
 *     -> PI(id) + PI(iq) -> inverse Park -> min-max SVPWM
 *     -> TIM1 CH1/2/3 (+CHxN)
 *
 * Everything here is float; the G431 has an FPU and the ISR budget at
 * 20 kHz is ~8500 cycles, which is far more than this needs.
 */
#include "foc.h"
#include "encoder.h"

#include <math.h>
#include <stddef.h>

#define SQRT3_F         (1.73205080757f)
#define TWO_PI_F        (6.28318530718f)
#define INV_SQRT3_F     (0.57735026919f)

/* DRV8316 marks SOx positive for current flowing *into* the low-side FET.
 * Positive phase current (inverter -> motor) is therefore negative during
 * the low-side sampling window. Flip this if id/iq come out inverted. */
#ifndef FOC_CURRENT_SIGN
#define FOC_CURRENT_SIGN    (-1.0f)
#endif

/* -------------------------------------------------------------------------- */
/* state                                                                      */
/* -------------------------------------------------------------------------- */
static TIM_HandleTypeDef *s_htim;

static FocPi s_pi_d = { FOC_KP, FOC_KI, 0.0f };
static FocPi s_pi_q = { FOC_KP, FOC_KI, 0.0f };

static int32_t s_off[3];            /* zero-current ADC counts per phase */
static float   s_id;
static float   s_iq;
static float   s_id_ref;
static float   s_iq_ref;
static uint8_t s_ready;
static uint8_t s_enabled;

/* -------------------------------------------------------------------------- */
/* helpers                                                                    */
/* -------------------------------------------------------------------------- */
static inline float clampf(float x, float lo, float hi)
{
    if (x < lo) { return lo; }
    if (x > hi) { return hi; }
    return x;
}

/* Write phase duties. Board mapping is fixed by the PCB:
 *   TIM1_CH3 -> INHA -> OUTA
 *   TIM1_CH2 -> INHB -> OUTB
 *   TIM1_CH1 -> INHC -> OUTC
 */
static void write_duties(float da, float db, float dc)
{
    uint32_t period = FOC_PWM_ARR + 1u;

    da = clampf(da, FOC_DUTY_MIN, FOC_DUTY_MAX);
    db = clampf(db, FOC_DUTY_MIN, FOC_DUTY_MAX);
    dc = clampf(dc, FOC_DUTY_MIN, FOC_DUTY_MAX);

    __HAL_TIM_SET_COMPARE(s_htim, TIM_CHANNEL_3, (uint32_t)(da * (float)period));
    __HAL_TIM_SET_COMPARE(s_htim, TIM_CHANNEL_2, (uint32_t)(db * (float)period));
    __HAL_TIM_SET_COMPARE(s_htim, TIM_CHANNEL_1, (uint32_t)(dc * (float)period));
}

/* min-max (zero-sequence) SVPWM. Adds 15 % of linear range vs. sine PWM
 * and, more importantly here, centres the three duties so the low-side
 * sampling window at the counter peak stays usable at high modulation. */
static void svpwm(float va, float vb, float vc,
                  float *da, float *db, float *dc)
{
    float vmax = va;
    float vmin = va;
    float off;

    if (vb > vmax) { vmax = vb; }
    if (vc > vmax) { vmax = vc; }
    if (vb < vmin) { vmin = vb; }
    if (vc < vmin) { vmin = vc; }

    off = -0.5f * (vmax + vmin);

    *da = 0.5f + (va + off) / FOC_VBUS;
    *db = 0.5f + (vb + off) / FOC_VBUS;
    *dc = 0.5f + (vc + off) / FOC_VBUS;
}

/* -------------------------------------------------------------------------- */
/* init / enable                                                              */
/* -------------------------------------------------------------------------- */
void FOC_Init(TIM_HandleTypeDef *htim1)
{
    s_htim = htim1;

    /* CH4 is used only as the ADC trigger; no pin is driven.
     * PWM mode 2 on OC4REF gives a rising edge at CNT == CCR4 on the
     * up-count. With CCR4 = ARR-1 that is the counter peak, where all
     * three low-side FETs are on (the low-side shunt sampling window).
     * CR2.MMS = 111 routes OC4REF to the ADC trigger. */
    if (s_htim != NULL)
    {
        s_htim->Instance->CCER  &= ~TIM_CCER_CC4E;
        s_htim->Instance->CCMR2  = (s_htim->Instance->CCMR2 & ~TIM_CCMR2_OC4M_Msk) |
                                   TIM_CCMR2_OC4M_2 | TIM_CCMR2_OC4M_1 | TIM_CCMR2_OC4M_0;
        s_htim->Instance->CCR4   = FOC_PWM_ARR - 1u;
        s_htim->Instance->CR2    = (s_htim->Instance->CR2 & ~TIM_CR2_MMS_Msk) |
                                   TIM_TRGO_OC4REF;
    }

    s_pi_d.kp = FOC_KP;
    s_pi_d.ki = FOC_KI;
    s_pi_d.integ = 0.0f;
    s_pi_q.kp = FOC_KP;
    s_pi_q.ki = FOC_KI;
    s_pi_q.integ = 0.0f;

    s_id = 0.0f;
    s_iq = 0.0f;
    s_id_ref = 0.0f;
    s_iq_ref = 0.0f;

    s_enabled = 0u;
    s_ready = 0u;
}

void FOC_CalibrateOffsets(const volatile uint16_t *adc, uint32_t n)
{
    uint64_t sa = 0u;
    uint64_t sb = 0u;
    uint64_t sc = 0u;

    if ((adc == NULL) || (n == 0u))
    {
        return;
    }

    for (uint32_t i = 0u; i < n; i++)
    {
        sa += adc[0];
        sb += adc[1];
        sc += adc[2];
        HAL_Delay(1u);
    }

    s_off[0] = (int32_t)(sa / n);
    s_off[1] = (int32_t)(sb / n);
    s_off[2] = (int32_t)(sc / n);

    s_ready = 1u;
}

void FOC_Enable(void)
{
    if ((s_htim == NULL) || (s_ready == 0u))
    {
        return;
    }

    /* Refuse to commutate without a valid encoder offset: the default
     * sign/offset would make the motor vibrate or run away. */
    if (Encoder_GetCal()->valid == 0u)
    {
        return;
    }

    s_pi_d.integ = 0.0f;
    s_pi_q.integ = 0.0f;

    /* Safe 50 % before MOE is released (MOE turns on with the first
     * HAL_TIM_PWM_Start below). */
    write_duties(0.5f, 0.5f, 0.5f);

    /* Start the three main outputs and their complements, then release MOE. */
    (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_2);
    (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_3);

    (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_1);
    (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_2);
    (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_3);

    s_id_ref = 0.0f;
    s_iq_ref = 0.0f;
    s_enabled = 1u;
}

void FOC_Disable(void)
{
    s_enabled = 0u;
    s_id_ref = 0.0f;
    s_iq_ref = 0.0f;
    s_pi_d.integ = 0.0f;
    s_pi_q.integ = 0.0f;

    if (s_htim != NULL)
    {
        __HAL_TIM_MOE_DISABLE(s_htim);
    }
}

uint8_t FOC_IsReady(void)
{
    return s_ready;
}

uint8_t FOC_IsEnabled(void)
{
    return s_enabled;
}

void FOC_SetCurrentRef(float id, float iq)
{
    /* Circular current limit. The DRV8316 CSA (0.15 V/A) and the motor are
     * both only good to ~7 A peak, so clamp the dq vector magnitude. */
    float m2 = (id * id) + (iq * iq);

    if (m2 > (FOC_I_MAX * FOC_I_MAX))
    {
        float s = FOC_I_MAX / sqrtf(m2);
        id *= s;
        iq *= s;
    }

    s_id_ref = id;
    s_iq_ref = iq;
}

/* -------------------------------------------------------------------------- */
/* control ISR                                                                */
/* -------------------------------------------------------------------------- */
void FOC_ControlISR(const volatile uint16_t *adc)
{
    float ia, ib, ic;
    float alpha, beta;
    float th, ct, st;
    float id, iq;
    float ed, eq;
    float vd, vq;
    float valpha, vbeta;
    float va, vb, vc;
    float mag2;
    float da, db, dc;

    if ((s_htim == NULL) || (s_ready == 0u) || (s_enabled == 0u))
    {
        s_id = 0.0f;
        s_iq = 0.0f;
        return;
    }

    Encoder_Tick();                     /* non-blocking I2C, every 5th call */
    /* ---- 1. currents ----------------------------------------------------- */
    ia = FOC_CURRENT_SIGN * (float)((int32_t)adc[0] - s_off[0]) * FOC_I_PER_COUNT;
    ib = FOC_CURRENT_SIGN * (float)((int32_t)adc[1] - s_off[1]) * FOC_I_PER_COUNT;
    ic = FOC_CURRENT_SIGN * (float)((int32_t)adc[2] - s_off[2]) * FOC_I_PER_COUNT;

    /* ---- 2. Clarke (amplitude invariant, uses all three channels) -------- */
    alpha = ia;
    beta  = (ib - ic) * INV_SQRT3_F;

    /* ---- 3. Park --------------------------------------------------------- */
    th = Encoder_ElectricalAngle();
    ct = cosf(th);
    st = sinf(th);

    id =  alpha * ct + beta * st;
    iq = -alpha * st + beta * ct;

    s_id = id;
    s_iq = iq;

    /* ---- 4. current PI with back-calculation anti-windup ----------------- */
    ed = s_id_ref - id;
    eq = s_iq_ref - iq;

    s_pi_d.integ += FOC_KI * FOC_TS * ed;
    s_pi_q.integ += FOC_KI * FOC_TS * eq;

    s_pi_d.integ = clampf(s_pi_d.integ, -FOC_VMAX, FOC_VMAX);
    s_pi_q.integ = clampf(s_pi_q.integ, -FOC_VMAX, FOC_VMAX);

    vd = s_pi_d.kp * ed + s_pi_d.integ;
    vq = s_pi_q.kp * eq + s_pi_q.integ;

    /* Circular voltage limit: shrink the vector, keep its direction, and
     * fold the result back into the integrators so they do not wind up. */
    mag2 = vd * vd + vq * vq;
    if (mag2 > (FOC_VMAX * FOC_VMAX))
    {
        float s = FOC_VMAX / sqrtf(mag2);
        vd *= s;
        vq *= s;
        s_pi_d.integ = vd - s_pi_d.kp * ed;
        s_pi_q.integ = vq - s_pi_q.kp * eq;
    }

    /* ---- 5. inverse Park -> inverse Clarke ------------------------------- */
    valpha = vd * ct - vq * st;
    vbeta  = vd * st + vq * ct;

    va = valpha;
    vb = -0.5f * valpha + (0.5f * SQRT3_F) * vbeta;
    vc = -0.5f * valpha - (0.5f * SQRT3_F) * vbeta;

    /* ---- 6. SVPWM -> TIM1 ------------------------------------------------- */
    svpwm(va, vb, vc, &da, &db, &dc);
    write_duties(da, db, dc);
}

/* -------------------------------------------------------------------------- */
/* open-loop vector (encoder calibration only)                                */
/* -------------------------------------------------------------------------- */
void FOC_ApplyOpenLoopVector(float theta_e, float modulation)
{
    float ct, st, amp;
    float va, vb, vc;
    float da, db, dc;

    if (s_htim == NULL)
    {
        return;
    }

    if (modulation <= 0.0f)
    {
        FOC_Disable();
        return;
    }

    /* Safety: at standstill I = mod*Vbus/Rph, and this motor has Rph = 0.109 ohm.
     * A 4 % modulation would be ~8.8 A, above the motor and CSA ratings. */
    if (modulation > FOC_ALIGN_MOD_MAX)
    {
        modulation = FOC_ALIGN_MOD_MAX;
    }

    ct  = cosf(theta_e);
    st  = sinf(theta_e);
    amp = modulation * FOC_VBUS;

    va = amp * ct;
    vb = amp * (-0.5f * ct + (0.5f * SQRT3_F) * st);
    vc = amp * (-0.5f * ct - (0.5f * SQRT3_F) * st);

    /* Write the vector first, then release MOE (avoids a stale duty pulse). */
    svpwm(va, vb, vc, &da, &db, &dc);
    write_duties(da, db, dc);

    if (s_enabled == 0u)
    {
        (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_1);
        (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_2);
        (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_3);
        (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_1);
        (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_2);
        (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_3);
    }
}

/* -------------------------------------------------------------------------- */
/* telemetry                                                                  */
/* -------------------------------------------------------------------------- */
float FOC_GetId(void)
{
    return s_id;
}

float FOC_GetIq(void)
{
    return s_iq;
}

const FocPi *FOC_GetPiD(void)
{
    return &s_pi_d;
}

const FocPi *FOC_GetPiQ(void)
{
    return &s_pi_q;
}
