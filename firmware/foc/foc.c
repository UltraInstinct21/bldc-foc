/**
 * @file    foc.c
 * @brief   Minimal sensored FOC for the DRV8316 board.
 *
 * Signal chain per 20 kHz cycle:
 *
 *   ADC (3 low-side shunts)
 *     -> ia/ib/ic -> Clarke -> Park(theta from encoder)
 *     -> PI(id) + PI(iq) -> inverse Park -> DPWMMIN (or SVPWM)
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

/* Measured on the board (bring-up raw-duty test, 9 V, stalled motor):
 * "raw A=10 % B=0 C=0" read A = +2.9 A and B, C = -1.6 A with
 * i = (V_SOx - offset) / 0.3. So SOx rises with current flowing out of the
 * phase into the motor, which is the FOC convention: the sign is +1. */
#ifndef FOC_CURRENT_SIGN
#define FOC_CURRENT_SIGN    (1.0f)
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
static uint8_t s_maxph;             /* phase with the highest duty last cycle */

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

    da = clampf(da, 0.0f, FOC_DUTY_MAX);
    db = clampf(db, 0.0f, FOC_DUTY_MAX);
    dc = clampf(dc, 0.0f, FOC_DUTY_MAX);

    /* Shortest low-side window next cycle: that phase is rebuilt by KCL. */
    s_maxph = (da >= db) ? ((da >= dc) ? 0u : 2u) : ((db >= dc) ? 1u : 2u);

    __HAL_TIM_SET_COMPARE(s_htim, TIM_CHANNEL_3, (uint32_t)(da * (float)period));
    __HAL_TIM_SET_COMPARE(s_htim, TIM_CHANNEL_2, (uint32_t)(db * (float)period));
    __HAL_TIM_SET_COMPARE(s_htim, TIM_CHANNEL_1, (uint32_t)(dc * (float)period));
}

/* Phase voltages (V) -> duties.
 *
 * Default DPWMMIN: lowest phase at exactly 0 %, i.e. its low side is held on
 * with no edges. Required while DRV8316 pin 35 (OCP/SR) is floating, see
 * foc.h. FOC_USE_SVPWM=1 selects min-max SVPWM for a board with pin 35
 * tied to GND. Both are linear up to |v_ll| = Vbus. */
static void modulate(float va, float vb, float vc,
                     float *da, float *db, float *dc)
{
#if FOC_USE_SVPWM
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
#else
    float d[3];

    Board_DutyDpwmMin(va, vb, vc, FOC_VBUS, d);
    *da = d[0];
    *db = d[1];
    *dc = d[2];
#endif
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

int FOC_CalibrateOffsets(const volatile uint16_t *adc, uint32_t n)
{
    const int32_t lo = (int32_t)(DRV8316_CSA_OFF_MIN_V / FOC_ADC_LSB);
    const int32_t hi = (int32_t)(DRV8316_CSA_OFF_MAX_V / FOC_ADC_LSB);
    uint64_t sa = 0u;
    uint64_t sb = 0u;
    uint64_t sc = 0u;

    s_ready = 0u;
    if ((adc == NULL) || (n == 0u))
    {
        return -1;
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

    for (uint32_t k = 0u; k < 3u; k++)
    {
        if ((s_off[k] < lo) || (s_off[k] > hi))
        {
            return -1;          /* expect ~VREF/2 = 1.65 V; not usable */
        }
    }

    s_ready = 1u;
    return 0;
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

    /* Zero vector before MOE is released (MOE turns on with the first
     * HAL_TIM_PWM_Start below). With DPWMMIN that is all duties 0. */
    {
        float da, db, dc;

        modulate(0.0f, 0.0f, 0.0f, &da, &db, &dc);
        write_duties(da, db, dc);
    }

    /* Start the three main outputs and their complements, then release MOE. */
    (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_1);
    (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_2);
    (void)HAL_TIM_PWM_Start(s_htim, TIM_CHANNEL_3);

    (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_1);
    (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_2);
    (void)HAL_TIMEx_PWMN_Start(s_htim, TIM_CHANNEL_3);

    /* HAL_TIM_PWM_Start() returns HAL_ERROR without touching MOE when the
     * channels are already started, which is the case after FOC_Disable()
     * (it only clears MOE). Set MOE explicitly so re-enable works, e.g.
     * foc_esp's ENABLE after a link-watchdog disable. */
    __HAL_TIM_MOE_ENABLE(s_htim);

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
    /* Circular current limit. The DRV8316T CSA (0.3 V/A) is linear to
     * +/-4.6 A peak, so clamp the dq vector magnitude to FOC_I_MAX (4.0 A). */
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

    /* SOx is valid only while that phase's low side is on. The phase with
     * the highest duty has the shortest window: rebuild it by KCL. */
    if (s_maxph == 0u)      { ia = -(ib + ic); }
    else if (s_maxph == 1u) { ib = -(ia + ic); }
    else                    { ic = -(ia + ib); }

    /* ---- 2. Clarke (amplitude invariant) ------------------------------- */
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

    /* ---- 6. modulation -> TIM1 -------------------------------------------- */
    modulate(va, vb, vc, &da, &db, &dc);
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

    /* Safety: at standstill only R limits the current (0.109 ohm motor).
     * See FOC_ALIGN_MOD_MAX in foc.h for the dead-time floor. */
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
    modulate(va, vb, vc, &da, &db, &dc);
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
