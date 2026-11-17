/**
 * @file    main.c
 * @brief   test_08_drive_lab — interactive drive-mode lab over the USB console.
 *
 * One app to compare every way of driving the motor on this board, using
 * only the DRV8316 current-sense outputs (SOA/SOB/SOC) as feedback:
 *
 *   open loop   6step (trapezoidal), sine, svpwm, dpwm (bottom-clamped)
 *   raw         fixed duties on all three phases, per-phase current
 *   rl          phase resistance and inductance from SOx (current loop)
 *   if          I/f: closed-loop dq current control, open-loop angle;
 *               also measures the flux linkage (lambda) from back-EMF
 *   sl          sensorless FOC: I/f start, flux observer + PLL, handover,
 *               speed loop
 *
 * Everything runs in the 20 kHz ADC DMA interrupt (ADC triggered at the TIM1
 * counter peak, where all low sides are on). Feedback quality is logged per
 * phase: RMS, peak, KCL residual (ia + ib + ic, all three valid) and the
 * fraction of samples with current flowing.
 *
 * Board facts this depends on (see board.h / docs/control.md): DRV8316 pin 35
 * floats (ASR/AAR on), so sine/svpwm are expected to pass little current;
 * 6-step and dpwm keep one low side on. 1 us dead time. No VM sense: set the
 * supply with "vbus <V>" (default 9 V).
 *
 * Safety: phase-current limiter (open loop) or current references (FOC)
 * bounded by ilim; trip at min(2*ilim, 5.3 A) filtered for 20 ms; nFAULT
 * (2 ms) stops; "voltage without current" interlock in open loop; ESC/Ctrl-C
 * or "stop" at any time. Type "help" on the console.
 */
#include "board.h"
#include "debug.h"
#include "usb_console.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FS_HZ           20000.0f
#define TS              (1.0f / FS_HZ)
#define PI_F            3.14159265f
#define TWO_PI_F        6.28318531f
#define INV_SQRT3       0.57735027f
#define SQRT3_2         0.86602540f
#define K_I             ((3.3f / 4095.0f) / DRV8316_CSA_V_PER_A)
#define D_VALID_MAX     0.85f       /* SOx valid if duty below this         */
#define D_MAX           0.98f
#define DT_FRAC         0.02f       /* 1 us dead time / 50 us period        */
#define I_TRIP_MAX      5.3f

typedef enum { M_6STEP = 0, M_SINE, M_SVPWM, M_DPWM } Mod;
typedef enum { S_IDLE = 0, S_ALIGN, S_RUN, S_RAW, S_RL, S_FOC } State;
typedef enum { F_IF = 0, F_SL } FocKind;

static const char *const k_mod[] = { "6step", "sine", "svpwm", "dpwm" };

/* ---- parameters (console) ------------------------------------------------ */
static float p_vbus     = 9.0f;
static float p_ilim     = 2.0f;     /* A, phase peak                         */
static float p_amp      = 40.0f;    /* %: 6step duty, else l-l peak / Vbus   */
static float p_f        = 200.0f;   /* Hz electrical, open-loop target       */
static float p_fstart   = 4.0f;
static float p_acc0     = 5.0f;     /* Hz/s                                  */
static float p_acc1     = 0.1f;     /* 1/s                                   */
static uint32_t p_align = 500u;     /* ms                                    */
static float p_R        = 0.0f;     /* ohm, phase (from "rl" or "set R")     */
static float p_L        = 0.0f;     /* H, phase                              */
static float p_lam      = 0.0f;     /* Wb (V*s), 0 = take from I/f estimate  */
static float p_bw       = 400.0f;   /* current loop bandwidth, Hz            */
static float p_ifa      = 1.5f;     /* I/f current, A                        */
static float p_fsw      = 35.0f;    /* sensorless handover, Hz electrical    */
static float p_kps      = 0.01f;    /* speed loop, A per Hz                  */
static float p_kis      = 0.05f;    /* A per Hz per s                        */
static float p_pll      = 30.0f;    /* PLL bandwidth, Hz                     */
/* d-axis current held in sensorless mode. With pin 35 floating the DRV8316
 * shuts a half-bridge off at low current (ASR/AAR), so an unloaded motor at
 * id = 0 (a few hundred mA) stops conducting. -1 A keeps |I| >= 1 A at the
 * cost of a little field weakening. */
static float p_idsl     = -1.0f;
/* Observer voltage source: 0 = commanded alpha/beta, 1 = written duties x Vbus
 * (default; best in the offline replay of a 35 Hz trace), 2 = 1 + floating-
 * phase reconstruction. */
static uint8_t p_vrec   = 1u;
/* Dead-time compensation (+/-2 % by current sign on switching phases). On
 * this board it measurably reduces the zero-current dropouts (0 % vs 28-40 %
 * at 27-35 Hz in I/f), so it stays on. The observer uses the written duties
 * (vrec 1), which include it: that combination tracked the rotor in the
 * offline replay of a 35 Hz trace. */
static uint8_t p_dtc    = 1u;
static Mod   p_focmod   = M_DPWM;
static uint32_t p_log   = 500u;     /* ms                                    */

/* ---- shared with the ISR ------------------------------------------------- */
static volatile uint16_t g_dma[ADC_BUF_LEN];
static volatile State    s_state;
static volatile Mod      s_mod;
static volatile FocKind  s_kind;
static const char *volatile s_stop_reason;
static float    s_off[3];
static uint32_t s_arr;
static float    s_d_app[3];         /* duties in effect for the next sample  */
static uint8_t  s_step = 0xFFu;

static volatile float s_theta, s_freq, s_amp_cur, s_amp_app, s_cap;
static volatile float s_ifilt, s_fmax_seen, s_imax_seen;
static volatile uint32_t s_state_ms;

/* per-phase stats, reset by the logger */
typedef struct {
    uint32_t n, n_res, n_flow;
    float    sq[3], pk[3], res_sq;
    uint32_t nv[3];
    float    sum[3];                 /* signed, valid samples (raw test)     */
    uint32_t nfl[3];                 /* valid samples with |i| > 0.1 A       */
} Stats;
static volatile Stats s_st;

/* FOC */
typedef struct { float kp, ki, integ; } Pi;
static Pi    s_pid, s_piq;
static volatile float s_idr, s_iqr, s_id, s_iq, s_vd, s_vq;
static volatile uint32_t s_dropouts;     /* samples with |I| < 30 % of ref */
static volatile float s_theta_ol, s_f_ol;
static float  s_xa, s_xb;            /* observer flux state                  */
static volatile float s_th_obs, s_th_pll, s_w_pll, s_wf;  /* s_wf: filtered */
static volatile float s_lam_est, s_lam_use;
static volatile uint8_t s_obs_on, s_obs_lock_ms_ok;
static volatile uint32_t s_lock_ms;
static Pi    s_pis;                  /* speed                                */
static float s_va_prev, s_vb_prev;   /* alpha/beta voltage applied           */

/* fault trace: last 512 samples, frozen when the bridge stops */
typedef struct { int16_t i[3]; uint16_t d[3]; uint16_t th; uint16_t tho; uint16_t flx; uint8_t nf; uint8_t sec; } Tr;
static uint32_t p_trdec = 1u;       /* record every n-th sample */
#define TR_N 512u
static Tr s_tr[TR_N];
static volatile uint32_t s_tr_pos;
static volatile uint8_t  s_tr_frozen;

/* R/L measurement */
static volatile uint32_t s_rl_n;
static volatile uint8_t  s_rl_stage;
static volatile float s_rl_acc_v, s_rl_acc_i, s_rl_v[2], s_rl_i[2];
static volatile float s_rl_s, s_rl_c, s_rl_ih[2];
static float s_rl_vd;
#define RL_I1      1.0f
#define RL_KI      60.0f             /* V/(A*s), integral-only current loop  */
#define RL_VH      0.25f             /* injection amplitude, V               */
static const float k_rl_f[2] = { 1000.0f, 2000.0f };

/* ======================================================================== */
static float clampf(float x, float lo, float hi)
{
    return (x < lo) ? lo : ((x > hi) ? hi : x);
}

static float wrap2pi(float a)
{
    while (a >= TWO_PI_F) { a -= TWO_PI_F; }
    while (a < 0.0f)      { a += TWO_PI_F; }
    return a;
}

static float wrap_pm_pi(float a)
{
    while (a > PI_F)  { a -= TWO_PI_F; }
    while (a < -PI_F) { a += TWO_PI_F; }
    return a;
}

/* ---- TIM1 output modes --------------------------------------------------- */
#define OCM_FORCE_LOW 4u
#define OCM_PWM1      6u
enum { P = 0, L = 1, Z = 2 };
static const uint8_t k_steps[6][3] = {
    { P, L, Z }, { P, Z, L }, { Z, P, L }, { L, P, Z }, { L, Z, P }, { Z, L, P },
};
static const uint8_t k_ch[3] = { 3u, 2u, 1u };      /* A = CH3, B = CH2, C = CH1 */

static void set_ocm(uint32_t ch, uint32_t mode)
{
    volatile uint32_t *ccmr = (ch <= 2u) ? &TIM1->CCMR1 : &TIM1->CCMR2;
    uint32_t sh  = ((ch & 1u) != 0u) ? 4u : 12u;
    uint32_t msk = (7u << sh) | (1u << (sh + 12u));

    *ccmr = (*ccmr & ~msk) | (mode << sh);
}

#define CCER_ALL (TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E | TIM_CCER_CC2NE | \
                  TIM_CCER_CC3E | TIM_CCER_CC3NE)

static void load_step(uint8_t s)
{
    uint32_t ccer = TIM1->CCER & ~CCER_ALL;

    for (uint32_t p = 0u; p < 3u; p++)
    {
        uint32_t ch = k_ch[p];
        uint32_t en = (1u << (4u * (ch - 1u))) | (1u << (4u * (ch - 1u) + 2u));

        if (k_steps[s][p] == P)      { set_ocm(ch, OCM_PWM1);      ccer |= en; }
        else if (k_steps[s][p] == L) { set_ocm(ch, OCM_FORCE_LOW); ccer |= en; }
    }
    TIM1->CCER = ccer;
    TIM1->EGR  = TIM_EGR_COMG;
}

static void set_pwm_all(void)
{
    for (uint32_t ch = 1u; ch <= 3u; ch++) { set_ocm(ch, OCM_PWM1); }
    TIM1->CCER |= CCER_ALL;
    TIM1->EGR   = TIM_EGR_COMG;
}

/* duties in phase order A, B, C */
static void write_duties(const float d[3])
{
    float per = (float)(s_arr + 1u);

    for (uint32_t k = 0u; k < 3u; k++)
    {
        s_d_app[k] = clampf(d[k], 0.0f, D_MAX);
    }
    TIM1->CCR3 = (uint32_t)(s_d_app[0] * per);
    TIM1->CCR2 = (uint32_t)(s_d_app[1] * per);
    TIM1->CCR1 = (uint32_t)(s_d_app[2] * per);
}

/* phase voltages normalised to Vbus -> duties, for any continuous mode */
static void modulate(Mod m, float va, float vb, float vc, float d[3])
{
    float vmax = fmaxf(va, fmaxf(vb, vc));
    float vmin = fminf(va, fminf(vb, vc));

    switch (m)
    {
    case M_SINE:
        d[0] = 0.5f + va; d[1] = 0.5f + vb; d[2] = 0.5f + vc;
        break;
    case M_SVPWM:
    {
        float off = 0.5f - 0.5f * (vmax + vmin);
        d[0] = va + off; d[1] = vb + off; d[2] = vc + off;
        break;
    }
    default:                                /* DPWM, bottom clamped */
        d[0] = va - vmin; d[1] = vb - vmin; d[2] = vc - vmin;
        break;
    }
}

static void bridge_stop(const char *why)
{
    __disable_irq();
    Board_BridgeOff();
    TIM1->CCER &= ~CCER_ALL;
    TIM1->EGR   = TIM_EGR_COMG;
    TIM1->CCR1 = TIM1->CCR2 = TIM1->CCR3 = 0u;
    s_d_app[0] = s_d_app[1] = s_d_app[2] = 0.0f;
    s_step = 0xFFu;
    if ((s_state != S_IDLE) && (s_stop_reason == NULL))
    {
        s_stop_reason = why;
        s_tr_frozen = 1u;
    }
    s_state = S_IDLE;
    __enable_irq();
}

/* ---- per-sample feedback ------------------------------------------------- */
static void stats_add(const float ir[3], const float i[3], uint8_t all_valid,
                      const uint8_t valid[3])
{
    Stats *st = (Stats *)&s_st;
    float mag2 = 0.0f;

    st->n++;
    for (uint32_t k = 0u; k < 3u; k++)
    {
        float a = fabsf(i[k]);

        st->sq[k] += i[k] * i[k];
        if (a > st->pk[k]) { st->pk[k] = a; }
        mag2 += i[k] * i[k];
        if (valid[k] != 0u)
        {
            st->nv[k]++;
            st->sum[k] += ir[k];
            if (fabsf(ir[k]) > 0.1f) { st->nfl[k]++; }
        }
    }
    if (mag2 > 0.03f) { st->n_flow++; }      /* |I| > ~0.14 A */
    if (all_valid != 0u)
    {
        float r = ir[0] + ir[1] + ir[2];

        st->res_sq += r * r;
        st->n_res++;
    }
}

/* ---- FOC helpers ---------------------------------------------------------- */
static float pi_run(Pi *c, float e, float lim, uint8_t hold)
{
    float u;

    /* hold = current dropout: do not integrate, or the PI winds up into the
     * high-duty region where the bridge keeps blocking (pin 35 floating) */
    c->integ = clampf(c->integ + ((hold != 0u) ? 0.0f : (c->ki * e * TS)), -lim, lim);
    u = c->kp * e + c->integ;
    return u;
}

/* dq voltage (V) at angle th -> duties, with optional dead-time comp */
static void apply_vdq(float vd, float vq, float th, const float i[3])
{
    float c = cosf(th), s = sinf(th);
    float va = vd * c - vq * s;
    float vb = vd * s + vq * c;
    float ph[3], d[3];
    float k = 1.0f / p_vbus;

    s_va_prev = va;
    s_vb_prev = vb;
    ph[0] = va * k;
    ph[1] = (-0.5f * va + SQRT3_2 * vb) * k;
    ph[2] = (-0.5f * va - SQRT3_2 * vb) * k;
    modulate((s_state == S_RL) ? M_DPWM : p_focmod, ph[0], ph[1], ph[2], d);

    if (p_dtc != 0u)
    {
        for (uint32_t n = 0u; n < 3u; n++)
        {
            if ((d[n] > 0.0f) && (d[n] < D_MAX))       /* switching phases only */
            {
                d[n] += DT_FRAC * clampf(i[n] / 0.2f, -1.0f, 1.0f);
            }
        }
    }
    write_duties(d);
}

static float vmax_mod(void)
{
    return ((p_focmod == M_SINE) ? 0.5f : (0.95f * INV_SQRT3)) * p_vbus;
}

static void observer_reset(float lam, float th)
{
    s_xa = lam * cosf(th);
    s_xb = lam * sinf(th);
}

/* Voltage actually on the motor over the last period, alpha/beta.
 * Built from the duties that were in effect (s_d_app, before this ISR writes
 * new ones), with the dead-time loss modelled explicitly. With pin 35
 * floating, the DRV8316 turns a half-bridge off near zero current, so a phase
 * whose current is stuck at ~0 is FLOATING: its terminal voltage is its own
 * back-EMF e_k, not the command, and the neutral moves to
 * (v_j + v_l + e_k) / 2. e_k comes from the observer flux estimate. */
static volatile uint32_t s_float_n;
static void applied_voltage(const float i[3], float *va, float *vb)
{
    float v[3], vn[3], e[3];
    int   fl = -1, nfl = 0;
    float iref2 = s_idr * s_idr + s_iqr * s_iqr;

    for (int k = 0; k < 3; k++)
    {
        v[k] = clampf(s_d_app[k], 0.0f, 1.0f) * p_vbus;   /* no dead-time loss: ASR */
        if ((iref2 > 0.09f) && (fabsf(i[k]) < 0.06f)) { fl = k; nfl++; }
    }
    if ((p_vrec == 2u) && (nfl == 1) && (s_lam_use > 0.0f))
    {
        float pa = s_xa - p_L * i[0];
        float pb = s_xb - p_L * (i[0] + 2.0f * i[1]) * INV_SQRT3;
        float ea = -s_w_pll * pb, eb = s_w_pll * pa;          /* e = w * J * psi */
        int j = (fl + 1) % 3, l = (fl + 2) % 3;
        float vN;

        e[0] = ea;
        e[1] = -0.5f * ea + SQRT3_2 * eb;
        e[2] = -0.5f * ea - SQRT3_2 * eb;
        vN = 0.5f * (v[j] + v[l] + e[fl]);
        vn[j] = v[j] - vN;
        vn[l] = v[l] - vN;
        vn[fl] = e[fl];
        s_float_n++;
    }
    else
    {
        float vN = (v[0] + v[1] + v[2]) * (1.0f / 3.0f);
        for (int k = 0; k < 3; k++) { vn[k] = v[k] - vN; }
    }
    *va = vn[0];
    *vb = (vn[0] + 2.0f * vn[1]) * INV_SQRT3;
}

/* Ortega non-linear flux observer + PLL. ia, ib = alpha/beta current now,
 * s_va_prev/s_vb_prev = voltage applied over the last period. */
static void observer_step(float ia, float ib, uint8_t dropout)
{
    float lam = s_lam_use;
    float fa, fb, err, gamma, e, kp, ki, wn;

    if (lam <= 0.0f)
    {
        return;
    }
    if (dropout != 0u)
    {
        /* The bridge is not conducting, so the commanded voltage is not on the
         * motor (terminals float at the back-EMF). Coast: rotate the rotor
         * flux estimate at the PLL speed instead of integrating v - R*i. */
        float da = s_w_pll * TS;
        float c = 1.0f - 0.5f * da * da, s = da;
        float xa = s_xa - p_L * ia, xb = s_xb - p_L * ib;
        float ra = xa * c - xb * s, rb = xa * s + xb * c;
        s_xa = ra + p_L * ia;
        s_xb = rb + p_L * ib;
    }
    else
    {
        fa = s_xa - p_L * ia;
        fb = s_xb - p_L * ib;
        err = lam * lam - (fa * fa + fb * fb);
        gamma = 2000.0f / (lam * lam);              /* ~1000 rad/s convergence */
        s_xa += (s_va_prev - p_R * ia + 0.5f * gamma * fa * err) * TS;
        s_xb += (s_vb_prev - p_R * ib + 0.5f * gamma * fb * err) * TS;
    }
    fa = s_xa - p_L * ia;
    fb = s_xb - p_L * ib;
    s_th_obs = wrap2pi(atan2f(fb, fa));

    wn = TWO_PI_F * p_pll;                       /* PLL */
    kp = 2.0f * 0.8f * wn;
    ki = wn * wn;
    e  = wrap_pm_pi(s_th_obs - s_th_pll);
    s_w_pll += ki * e * TS;
    s_th_pll = wrap2pi(s_th_pll + (s_w_pll + kp * e) * TS);
    s_wf += (TS / 0.02f) * (s_w_pll - s_wf);      /* 20 ms speed filter */
}

/* ---- 1 kHz supervision ---------------------------------------------------- */
static uint32_t s_over_ms, s_fault_ms, s_nocur_ms;

static uint8_t supervise_1ms(void)
{
    float trip = fminf(2.0f * p_ilim, I_TRIP_MAX);

    s_fault_ms = (Board_DrvFault() != 0u) ? (s_fault_ms + 1u) : 0u;
    if (s_fault_ms >= 2u)          { bridge_stop("nFAULT");      return 0u; }
    s_over_ms = (s_ifilt > trip) ? (s_over_ms + 1u) : 0u;
    if (s_over_ms >= 20u)          { bridge_stop("overcurrent"); return 0u; }
    return 1u;
}

static float slew(float x, float t, float step)
{
    if (x < t) { x += step; if (x > t) { x = t; } }
    else       { x -= step; if (x < t) { x = t; } }
    return x;
}

static void openloop_1ms(void)
{
    float d;

    if (supervise_1ms() == 0u) { return; }
    s_state_ms++;

    s_amp_cur = slew(s_amp_cur, p_amp, 0.05f);
    if (s_ifilt > p_ilim)
    {
        s_cap = fminf(s_cap, s_amp_app) - 0.1f;
        if (s_cap < 0.0f) { s_cap = 0.0f; }
    }
    else
    {
        s_cap = fminf(s_cap + 0.02f, s_amp_app + 2.0f);
    }
    d = fminf(s_amp_cur, s_cap);
    s_amp_app = d;

    /* "voltage without current": the measurement or the bridge is not working */
    if (s_mod != M_6STEP)
    {
        s_nocur_ms = ((s_amp_app > 10.0f) && (s_ifilt < 0.05f)) ? (s_nocur_ms + 1u) : 0u;
        if (s_nocur_ms >= 300u) { bridge_stop("no current at >10% amplitude"); return; }
    }

    if (s_state == S_ALIGN)
    {
        if (s_state_ms >= p_align) { s_state = S_RUN; s_freq = p_fstart; }
    }
    else
    {
        s_freq = slew(s_freq, p_f, (p_acc0 + p_acc1 * s_freq) * 0.001f);
    }
    if (s_freq > s_fmax_seen) { s_fmax_seen = s_freq; }
}

static void foc_1ms(void)
{
    if (supervise_1ms() == 0u) { return; }
    s_state_ms++;

    if (s_obs_on == 0u)
    {
        /* I/f: current along the open-loop angle */
        float iref = fminf(p_ifa, p_ilim);

        if (s_state_ms <= 200u)
        {
            s_idr = iref * (float)s_state_ms / 200.0f;
        }
        else if (s_state_ms > p_align + 200u)
        {
            float ftgt = (s_kind == F_IF) ? p_f : p_fsw;

            s_f_ol = slew(s_f_ol, ftgt, (p_acc0 + p_acc1 * s_f_ol) * 0.001f);
            if (s_f_ol > s_fmax_seen) { s_fmax_seen = s_f_ol; }
        }
        s_iqr = 0.0f;

        if (s_kind == F_SL)
        {
            /* take lambda from the I/f estimate once the BEMF is solid */
            if ((s_lam_use <= 0.0f) && (s_f_ol >= 0.8f * p_fsw))
            {
                s_lam_use = (p_lam > 0.0f) ? p_lam : s_lam_est;
                observer_reset(s_lam_use, s_theta_ol);
                s_th_pll = s_theta_ol;
                s_w_pll  = TWO_PI_F * s_f_ol;
                s_wf     = s_w_pll;
            }
            if ((s_lam_use > 0.0f) && (s_f_ol >= p_fsw))
            {
                float fp = s_wf / TWO_PI_F;

                s_lock_ms = (fabsf(fp - s_f_ol) < 0.15f * s_f_ol) ? (s_lock_ms + 1u) : 0u;
                if (s_lock_ms >= 200u)
                {
                    /* handover: express the I/f current in the observer frame */
                    float dth = wrap_pm_pi(s_theta_ol - s_th_pll);
                    float id0 = iref * cosf(dth), iq0 = iref * sinf(dth);

                    __disable_irq();
                    s_obs_on = 1u;
                    s_idr = id0;
                    s_iqr = iq0;
                    s_pis.integ = iq0;
                    /* rotate the PI states into the new frame too, and take the
                     * back-EMF out of the q integrator: it becomes feedforward */
                    {
                        float vd = s_pid.integ, vq = s_piq.integ;
                        s_pid.integ = vd * cosf(dth) - vq * sinf(dth);
                        s_piq.integ = vd * sinf(dth) + vq * cosf(dth) - s_w_pll * s_lam_use;
                    }
                    __enable_irq();
                }
                else if (s_state_ms > p_align + 200u + 15000u)
                {
                    bridge_stop("observer never locked");
                }
            }
        }
    }
    else
    {
        /* sensorless: speed loop on the filtered PLL speed */
        float fp = s_wf / TWO_PI_F;
        float e  = s_f_ol - fp;             /* s_f_ol is the speed reference now */
        float iq;

        s_f_ol = slew(s_f_ol, p_f, (p_acc0 + p_acc1 * s_f_ol) * 0.001f);
        s_idr  = slew(s_idr, p_idsl, 0.002f);
        s_pis.integ = clampf(s_pis.integ + p_kis * e * 0.001f, -p_ilim, p_ilim);
        iq = clampf(p_kps * e + s_pis.integ, -p_ilim, p_ilim);
        s_iqr = iq;
        if (fp > s_fmax_seen) { s_fmax_seen = fp; }
        if (fp < 0.3f * p_fsw)
        {
            if (++s_lock_ms > 100u) { bridge_stop("observer lost lock (speed collapsed)"); }
        }
        else
        {
            s_lock_ms = 0u;
        }
    }
}

/* ---- R/L measurement (ISR part) ------------------------------------------ */
static void rl_isr(float id, const float i[3])
{
    uint32_t n = s_rl_n++;
    float idr = (s_rl_stage == 0u) ? RL_I1 : fminf(2.0f * RL_I1, p_ilim);
    float vh = 0.0f;

    s_rl_vd = clampf(s_rl_vd + RL_KI * (idr - id) * TS, 0.0f, 0.25f * p_vbus);

    if (s_rl_stage <= 1u)
    {
        if (n >= 4000u) { s_rl_acc_v += s_rl_vd; s_rl_acc_i += id; }
        if (n >= 6000u)
        {
            s_rl_v[s_rl_stage] = s_rl_acc_v / 2000.0f;
            s_rl_i[s_rl_stage] = s_rl_acc_i / 2000.0f;
            s_rl_acc_v = s_rl_acc_i = 0.0f;
            s_rl_stage++;
            s_rl_n = 0u;
        }
    }
    else if (s_rl_stage <= 3u)
    {
        float w = TWO_PI_F * k_rl_f[s_rl_stage - 2u];
        float ph = w * (float)n * TS;
        vh = RL_VH * sinf(ph);
        /* lock-in on the current, after 1000 samples, over whole periods */
        if ((n >= 1000u) && (n < 5000u))
        {
            float phi = w * (float)(n - 1u) * TS;   /* voltage of the last period */
            s_rl_s += id * sinf(phi);
            s_rl_c += id * cosf(phi);
        }
        if (n >= 5000u)
        {
            s_rl_ih[s_rl_stage - 2u] = 2.0f * sqrtf(s_rl_s * s_rl_s + s_rl_c * s_rl_c) / 4000.0f;
            s_rl_s = s_rl_c = 0.0f;
            s_rl_stage++;
            s_rl_n = 0u;
        }
    }
    if (s_rl_stage >= 4u)
    {
        bridge_stop("rl done");
        return;
    }
    apply_vdq(s_rl_vd + vh, 0.0f, 0.0f, i);
}

/* ---- the 20 kHz loop ------------------------------------------------------ */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    static uint32_t div;
    float ir[3], i[3];
    uint8_t valid[3] = { 1u, 1u, 1u };
    uint8_t all_valid = 1u;
    float ia_ab, ib_ab, imag;
    State st = s_state;
    uint8_t tick1ms;

    if (hadc->Instance != ADC1)
    {
        return;
    }
    for (uint32_t k = 0u; k < 3u; k++)
    {
        ir[k] = ((float)g_dma[k] - s_off[k]) * K_I;
        i[k]  = ir[k];
    }
    if (st == S_IDLE)
    {
        return;
    }
    tick1ms = (++div >= 20u) ? 1u : 0u;
    if (tick1ms) { div = 0u; }

    /* ---- feedback: which SOx are valid this sample ---------------------- */
    if ((s_mod == M_6STEP) && ((st == S_ALIGN) || (st == S_RUN)))
    {
        uint8_t s = (s_step < 6u) ? s_step : 0u;

        for (uint32_t k = 0u; k < 3u; k++)
        {
            if (k_steps[s][k] == Z) { i[k] = 0.0f; valid[k] = 0u; all_valid = 0u; }
        }
    }
    else
    {
        uint8_t mx = (s_d_app[0] >= s_d_app[1]) ? ((s_d_app[0] >= s_d_app[2]) ? 0u : 2u)
                                                : ((s_d_app[1] >= s_d_app[2]) ? 1u : 2u);
        for (uint32_t k = 0u; k < 3u; k++)
        {
            if (s_d_app[k] >= D_VALID_MAX) { valid[k] = 0u; all_valid = 0u; }
        }
        i[mx] = -(i[(mx + 1u) % 3u] + i[(mx + 2u) % 3u]);
    }
    stats_add(ir, i, all_valid, valid);

    static uint32_t trd;
    if ((s_tr_frozen == 0u) && (++trd >= p_trdec))
    {
        Tr *t;
        trd = 0u;
        t = &s_tr[s_tr_pos++ & (TR_N - 1u)];
        for (uint32_t k = 0u; k < 3u; k++)
        {
            t->i[k] = (int16_t)(ir[k] * 1000.0f);               /* raw SOx, mA */
            t->d[k] = (uint16_t)(s_d_app[k] * 1000.0f);         /* 0.1 % */
        }
        t->th = (uint16_t)(((st == S_FOC) ? ((s_obs_on != 0u) ? s_th_pll : s_theta_ol) : s_theta)
                           * (3600.0f / TWO_PI_F));                /* 0.1 deg */
        t->tho = (uint16_t)(s_th_obs * (3600.0f / TWO_PI_F));
        {
            float fa = s_xa - p_L * i[0], fb = s_xb - p_L * (i[0] + 2.0f * i[1]) * INV_SQRT3;
            t->flx = (uint16_t)fminf(sqrtf(fa * fa + fb * fb) * 1e6f, 65535.0f);   /* uWb */
        }
        t->nf = (uint8_t)Board_DrvFault();
        t->sec = (uint8_t)valid[0] | (uint8_t)(valid[1] << 1) | (uint8_t)(valid[2] << 2);
    }

    ia_ab = i[0];
    ib_ab = (i[0] + 2.0f * i[1]) * INV_SQRT3;
    if ((s_mod == M_6STEP) && ((st == S_ALIGN) || (st == S_RUN)))
    {
        uint8_t s = (s_step < 6u) ? s_step : 0u;
        float il = 0.0f;
        for (uint32_t k = 0u; k < 3u; k++) { if (k_steps[s][k] == L) { il = ir[k]; } }
        imag = fabsf(il);
    }
    else
    {
        imag = sqrtf(ia_ab * ia_ab + ib_ab * ib_ab);
    }
    s_ifilt += 0.01f * (imag - s_ifilt);
    if (imag > s_imax_seen) { s_imax_seen = imag; }

    /* ---- states ---------------------------------------------------------- */
    switch (st)
    {
    case S_RAW:
        if (++s_state_ms >= 6000u) { bridge_stop("raw done"); }    /* 300 ms */
        else if (imag > 5.0f)      { bridge_stop("raw: >5 A, aborted"); }
        break;

    case S_RL:
    {
        float id = ia_ab;                      /* theta = 0 */
        if (tick1ms && (supervise_1ms() == 0u)) { break; }
        rl_isr(id, i);
        break;
    }

    case S_ALIGN:
    case S_RUN:
        if (tick1ms) { openloop_1ms(); if (s_state == S_IDLE) { break; } }
        if (s_state == S_RUN)
        {
            s_theta = wrap2pi(s_theta + TWO_PI_F * s_freq * TS);
        }
        if (s_mod == M_6STEP)
        {
            uint8_t step = (uint8_t)(s_theta * (3.0f / PI_F));
            float per = (float)(s_arr + 1u);
            uint32_t ccr = (uint32_t)(s_amp_app * 0.01f * per);

            if (step > 5u) { step = 5u; }
            if (step != s_step) { s_step = step; load_step(step); }
            TIM1->CCR1 = TIM1->CCR2 = TIM1->CCR3 = ccr;
        }
        else
        {
            float m = s_amp_app * 0.01f * INV_SQRT3;     /* phase peak / Vbus */
            float th = s_theta, d[3];

            modulate(s_mod, m * cosf(th), m * cosf(th - 2.0943951f),
                     m * cosf(th + 2.0943951f), d);
            write_duties(d);
        }
        break;

    case S_FOC:
    {
        float th, c, s, id, iq, ed, eq, vd, vq, mag, vlim;
        float w, iref2, imeas2;
        uint8_t hold;

        if (tick1ms) { foc_1ms(); if (s_state == S_IDLE) { break; } }

        if (s_obs_on == 0u)
        {
            s_theta_ol = wrap2pi(s_theta_ol + TWO_PI_F * s_f_ol * TS);
            th = s_theta_ol;
            w  = TWO_PI_F * s_f_ol;
        }
        else
        {
            th = s_th_pll;
            w  = s_w_pll;
        }
        c = cosf(th); s = sinf(th);
        id =  ia_ab * c + ib_ab * s;
        iq = -ia_ab * s + ib_ab * c;
        s_id = id; s_iq = iq;

        /* Physical voltage bound: what the motor can need at this speed,
         * R*I plus back-EMF plus margin. Keeps a current dropout from turning
         * into a full-voltage step when conduction returns. */
        {
            float lam = (s_lam_use > 0.0f) ? s_lam_use : fmaxf(s_lam_est, p_lam);
            if (lam <= 0.0f) { lam = 0.002f; }
            vlim = 2.0f * p_R * p_ilim + 1.5f * fabsf(w) * (lam + p_L * p_ilim) + 0.3f;
            vlim = fminf(vlim, vmax_mod());
        }
        iref2  = s_idr * s_idr + s_iqr * s_iqr;
        imeas2 = id * id + iq * iq;
        hold   = ((iref2 > 0.09f) && (imeas2 < 0.09f * iref2)) ? 1u : 0u;
        if (hold != 0u) { s_dropouts++; }

        ed = s_idr - id;
        eq = s_iqr - iq;
        vd = pi_run(&s_pid, ed, vlim, hold);
        vq = pi_run(&s_piq, eq, vlim, hold);
        vd -= w * p_L * iq;                        /* decoupling */
        vq += w * p_L * id;
        if (s_obs_on != 0u)
        {
            vq += w * s_lam_use;                   /* back-EMF feedforward */
        }
        mag = sqrtf(vd * vd + vq * vq);
        if (mag > vlim)
        {
            float k = vlim / mag;
            vd *= k; vq *= k;
        }
        s_vd = vd; s_vq = vq;

        /* lambda from back-EMF in the I/f frame (works for any lag angle) */
        if ((s_obs_on == 0u) && (s_f_ol > 10.0f))
        {
            float bd = vd - p_R * id + w * p_L * iq;
            float bq = vq - p_R * iq - w * p_L * id;
            float lam = sqrtf(bd * bd + bq * bq) / w;
            s_lam_est += 0.0005f * (lam - s_lam_est);
        }

        if (p_vrec != 0u)
        {
            float va, vb;
            applied_voltage(i, &va, &vb);
            s_va_prev = va;
            s_vb_prev = vb;
        }
        observer_step(ia_ab, ib_ab, hold);
        apply_vdq(vd, vq, th + 1.5f * w * TS, i);   /* 1.5-sample delay comp */
        break;
    }

    default:
        break;
    }
}

/* ======================================================================== */
static uint8_t csa_offsets(void)
{
    float sum[3] = { 0.0f, 0.0f, 0.0f };
    const float lsb = 3.3f / 4095.0f;
    uint8_t ok = 1u;

    for (uint32_t n = 0u; n < 64u; n++)
    {
        Board_DelayUs(100u);
        for (uint32_t k = 0u; k < 3u; k++) { sum[k] += (float)g_dma[k]; }
    }
    for (uint32_t k = 0u; k < 3u; k++)
    {
        float v;
        s_off[k] = sum[k] / 64.0f;
        v = s_off[k] * lsb;
        if ((v < DRV8316_CSA_OFF_MIN_V) || (v > DRV8316_CSA_OFF_MAX_V)) { ok = 0u; }
    }
    return ok;
}

static void stats_reset(void)
{
    __disable_irq();
    memset((void *)&s_st, 0, sizeof(s_st));
    __enable_irq();
}

static int prepare(void)
{
    bridge_stop("restart");
    Board_DelayMs(20u);
    if ((Board_DrvFault() != 0u) && (DRV8316_ClearFault() != 0))
    {
        printf("nFAULT low and would not clear (VIN?)\r\n");
        return -1;
    }
    if (csa_offsets() == 0u)
    {
        printf("CSA offsets out of range: DRV asleep or VM missing\r\n");
        return -1;
    }
    s_stop_reason = NULL;
    s_ifilt = 0.0f; s_imax_seen = 0.0f; s_fmax_seen = 0.0f;
    s_over_ms = s_fault_ms = s_nocur_ms = 0u;
    s_state_ms = 0u;
    stats_reset();
    s_tr_frozen = 0u;
    return 0;
}

static void start_openloop(void)
{
    float z[3] = { 0.0f, 0.0f, 0.0f };

    if (prepare() != 0) { return; }
    __disable_irq();
    s_mod = (Mod)s_mod;
    s_theta = 0.0f; s_freq = 0.0f;
    s_amp_cur = 0.0f; s_amp_app = 0.0f; s_cap = 2.0f;
    write_duties(z);
    if (s_mod == M_6STEP) { s_step = 0xFFu; }
    else                  { set_pwm_all(); }
    Board_DrvOutputEnable();
    TIM1->BDTR |= TIM_BDTR_MOE;
    s_state = S_ALIGN;
    __enable_irq();
    printf("start %s: amp %.0f%% f %.0f Hz ilim %.1f A\r\n",
           k_mod[s_mod], (double)p_amp, (double)p_f, (double)p_ilim);
}

static void start_raw(float a, float b, float c)
{
    float d[3] = { a * 0.01f, b * 0.01f, c * 0.01f };
    float lim = fmaxf(a, fmaxf(b, c)) - fminf(a, fminf(b, c));

    if (lim > 20.0f) { printf("raw: spread limited to 20%%\r\n"); return; }
    if (prepare() != 0) { return; }
    __disable_irq();
    write_duties(d);
    set_pwm_all();
    TIM1->BDTR |= TIM_BDTR_MOE;
    s_state = S_RAW;
    __enable_irq();
    while (s_state == S_RAW) { Board_DelayMs(5u); }
    {
        Stats st;
        __disable_irq(); memcpy(&st, (const void *)&s_st, sizeof(st)); __enable_irq();
        printf("RAW %5.1f %5.1f %5.1f | mean A %+6.2f B %+6.2f C %+6.2f A | flow%% A %3lu B %3lu C %3lu | %s\r\n",
               (double)a, (double)b, (double)c,
               (double)(st.nv[0] ? st.sum[0] / (float)st.nv[0] : 0.0f),
               (double)(st.nv[1] ? st.sum[1] / (float)st.nv[1] : 0.0f),
               (double)(st.nv[2] ? st.sum[2] / (float)st.nv[2] : 0.0f),
               (unsigned long)(st.nv[0] ? 100u * st.nfl[0] / st.nv[0] : 0u),
               (unsigned long)(st.nv[1] ? 100u * st.nfl[1] / st.nv[1] : 0u),
               (unsigned long)(st.nv[2] ? 100u * st.nfl[2] / st.nv[2] : 0u),
               s_stop_reason ? s_stop_reason : "");
    }
}

static void start_rl(void)
{
    float z[3] = { 0.0f, 0.0f, 0.0f };

    if (prepare() != 0) { return; }
    __disable_irq();
    s_rl_n = 0u; s_rl_stage = 0u; s_rl_vd = 0.0f;
    s_rl_acc_v = s_rl_acc_i = s_rl_s = s_rl_c = 0.0f;
    write_duties(z);
    set_pwm_all();
    TIM1->BDTR |= TIM_BDTR_MOE;
    s_state = S_RL;
    __enable_irq();
    printf("rl: holding 1 A then %.1f A at theta=0, then %.2f V injection at 1/2 kHz...\r\n",
           (double)fminf(2.0f * RL_I1, p_ilim), (double)RL_VH);
    while (s_state == S_RL) { Board_DelayMs(5u); }
    if ((s_rl_stage >= 4u) && (s_rl_i[1] > s_rl_i[0] + 0.2f))
    {
        float R = (s_rl_v[1] - s_rl_v[0]) / (s_rl_i[1] - s_rl_i[0]);
        float Ls[2];
        for (uint32_t k = 0u; k < 2u; k++)
        {
            float zz = (s_rl_ih[k] > 1e-3f) ? (RL_VH / s_rl_ih[k]) : 0.0f;
            float x2 = zz * zz - R * R;
            Ls[k] = (x2 > 0.0f) ? sqrtf(x2) / (TWO_PI_F * k_rl_f[k]) : 0.0f;
        }
        printf("RL: vd %.3f V @ %.2f A, %.3f V @ %.2f A -> R = %.4f ohm (phase, Vbus %.1f V)\r\n",
               (double)s_rl_v[0], (double)s_rl_i[0], (double)s_rl_v[1], (double)s_rl_i[1],
               (double)R, (double)p_vbus);
        printf("RL: |Ih| %.3f A @1kHz, %.3f A @2kHz -> L = %.1f / %.1f uH\r\n",
               (double)s_rl_ih[0], (double)s_rl_ih[1], (double)(Ls[0] * 1e6f), (double)(Ls[1] * 1e6f));
        if ((R > 0.005f) && (R < 5.0f)) { p_R = R; }
        if ((Ls[0] > 0.0f) && (Ls[1] > 0.0f)) { p_L = 0.5f * (Ls[0] + Ls[1]); }
        else if (Ls[1] > 0.0f) { p_L = Ls[1]; }
        printf("RL: using R = %.4f ohm, L = %.1f uH\r\n", (double)p_R, (double)(p_L * 1e6f));
    }
    else
    {
        printf("RL failed (%s, stage %u, i %.2f/%.2f A)\r\n",
               s_stop_reason ? s_stop_reason : "?", (unsigned)s_rl_stage,
               (double)s_rl_i[0], (double)s_rl_i[1]);
    }
}

static void start_foc(FocKind kind)
{
    float z[3] = { 0.0f, 0.0f, 0.0f };
    float wc = TWO_PI_F * p_bw;

    if ((p_R <= 0.0f) || (p_L <= 0.0f))
    {
        printf("run 'rl' first (or set R / set L)\r\n");
        return;
    }
    if (prepare() != 0) { return; }
    __disable_irq();
    s_kind = kind;
    s_pid.kp = s_piq.kp = p_L * wc;
    s_pid.ki = s_piq.ki = p_R * wc;
    s_pid.integ = s_piq.integ = 0.0f;
    s_pis.integ = 0.0f;
    s_idr = s_iqr = 0.0f;
    s_theta_ol = 0.0f; s_f_ol = 0.0f;
    s_obs_on = 0u; s_lock_ms = 0u;
    s_lam_est = 0.0f;
    s_lam_use = 0.0f;
    s_th_pll = 0.0f; s_w_pll = 0.0f; s_wf = 0.0f;
    s_va_prev = s_vb_prev = 0.0f;
    write_duties(z);
    set_pwm_all();
    TIM1->BDTR |= TIM_BDTR_MOE;
    s_state = S_FOC;
    __enable_irq();
    printf("start %s: I %.2f A, R %.4f ohm, L %.1f uH, Kp %.3f Ki %.1f, mod %s, dtc %u\r\n",
           (kind == F_IF) ? "I/f" : "sensorless", (double)fminf(p_ifa, p_ilim),
           (double)p_R, (double)(p_L * 1e6f), (double)s_pid.kp, (double)s_pid.ki,
           k_mod[p_focmod], (unsigned)p_dtc);
}

/* ---- logging -------------------------------------------------------------- */
static void log_line(uint32_t dt_ms)
{
    Stats st;
    float rms[3], res;
    uint32_t n;

    __disable_irq();
    memcpy(&st, (const void *)&s_st, sizeof(st));
    memset((void *)&s_st, 0, sizeof(s_st));
    __enable_irq();
    (void)dt_ms;
    n = (st.n > 0u) ? st.n : 1u;
    for (uint32_t k = 0u; k < 3u; k++) { rms[k] = sqrtf(st.sq[k] / (float)n); }
    res = (st.n_res > 0u) ? sqrtf(st.res_sq / (float)st.n_res) : -1.0f;

    if (s_state == S_FOC)
    {
        printf("%7.2f %s f_ol %6.1f f_pll %6.1f | id %+5.2f iq %+5.2f (ref %+5.2f %+5.2f) "
               "| v %4.2f V | lam %.5f use %.5f | rms %.2f %.2f %.2f | res %.2f | flow %3lu%% drop %3lu%% | %s\r\n",
               (double)Board_Millis() * 0.001, s_obs_on ? "SL " : "I/f",
               (double)s_f_ol, (double)(s_wf / TWO_PI_F),
               (double)s_id, (double)s_iq, (double)s_idr, (double)s_iqr,
               (double)sqrtf(s_vd * s_vd + s_vq * s_vq),
               (double)s_lam_est, (double)s_lam_use,
               (double)rms[0], (double)rms[1], (double)rms[2], (double)res,
               (unsigned long)(100u * st.n_flow / n),
               (unsigned long)(100u * s_dropouts / n), Board_DrvFault() ? "nF LOW" : "ok");
        printf("        floating-phase samples %3lu%%\r\n", (unsigned long)(100u * s_float_n / n));
        s_dropouts = 0u;
        s_float_n = 0u;
    }
    else
    {
        printf("%7.2f %-5s %-5s f %6.1f amp %5.1f%% | I %4.2f pk %4.2f | rms A %4.2f B %4.2f C %4.2f "
               "| res %5.2f | flow %3lu%% | %s\r\n",
               (double)Board_Millis() * 0.001, k_mod[s_mod],
               (s_state == S_ALIGN) ? "ALIGN" : "RUN",
               (double)s_freq, (double)s_amp_app, (double)s_ifilt,
               (double)fmaxf(st.pk[0], fmaxf(st.pk[1], st.pk[2])),
               (double)rms[0], (double)rms[1], (double)rms[2], (double)res,
               (unsigned long)(100u * st.n_flow / n), Board_DrvFault() ? "nF LOW" : "ok");
    }
}

static void trace_dump(uint32_t n)
{
    uint32_t end = s_tr_pos;

    if (n > TR_N) { n = TR_N; }
    printf("TRACE last %lu samples (every %lu x 50 us, newest last): iA iB iC [mA raw SOx] "
           "dA dB dC [0.1%%] theta theta_obs [0.1 deg] flux [uWb] valid nFAULT\r\n",
           (unsigned long)n, (unsigned long)p_trdec);
    for (uint32_t k = end - n; k != end; k++)
    {
        const Tr *t = &s_tr[k & (TR_N - 1u)];
        printf("%4ld %6d %6d %6d  %4u %4u %4u  %4u %4u %5u  %u%u%u %u\r\n",
               (long)(k - end), t->i[0], t->i[1], t->i[2], t->d[0], t->d[1], t->d[2], t->th,
               t->tho, t->flx,
               t->sec & 1u, (t->sec >> 1) & 1u, (t->sec >> 2) & 1u, t->nf);
    }
}

static void help(void)
{
    printf("commands:\r\n"
           "  mode 6step|sine|svpwm|dpwm   open-loop drive mode\r\n"
           "  start | stop                 open-loop run (align, ramp to f)\r\n"
           "  raw <A%%> <B%%> <C%%>          fixed duties 300 ms, per-phase current\r\n"
           "  rl                           measure R and L from SOx\r\n"
           "  if                           I/f: dq current loop, open-loop angle, lambda\r\n"
           "  sl                           sensorless FOC (I/f start, observer, speed loop)\r\n"
           "  amp <%%> f <Hz> ilim <A> vbus <V> accel <a0> <a1> align <ms> fstart <Hz>\r\n"
           "  set R|L|lam|bw|ifa|fsw|kps|kis|dtc|focmod <value>   (L in uH)\r\n"
           "  trace [n]                    last n samples before the last stop\r\n"
           "  log <ms> | params | help | dfu | ESC = stop\r\n");
}

static void params(void)
{
    printf("vbus %.1f V ilim %.2f A amp %.1f%% f %.1f Hz fstart %.1f accel %.1f+%.2f*f align %lu ms\r\n",
           (double)p_vbus, (double)p_ilim, (double)p_amp, (double)p_f, (double)p_fstart,
           (double)p_acc0, (double)p_acc1, (unsigned long)p_align);
    printf("R %.4f ohm L %.1f uH lam %.5f bw %.0f Hz ifa %.2f A fsw %.1f Hz kps %.3f kis %.3f dtc %u focmod %s mode %s\r\n",
           (double)p_R, (double)(p_L * 1e6f), (double)p_lam, (double)p_bw, (double)p_ifa,
           (double)p_fsw, (double)p_kps, (double)p_kis, (unsigned)p_dtc, k_mod[p_focmod], k_mod[s_mod]);
}

static int parse_mod(const char *s, Mod *m)
{
    for (uint32_t k = 0u; k < 4u; k++)
    {
        if (strcmp(s, k_mod[k]) == 0) { *m = (Mod)k; return 0; }
    }
    return -1;
}

static void command(char *line)
{
    char *argv[5] = { 0 };
    int argc = 0;
    char *tok = strtok(line, " ");

    while ((tok != NULL) && (argc < 5)) { argv[argc++] = tok; tok = strtok(NULL, " "); }
    if (argc == 0) { return; }

#define ARGF(k) ((argc > (k)) ? strtof(argv[k], NULL) : 0.0f)
    if      (!strcmp(argv[0], "help"))   { help(); }
    else if (!strcmp(argv[0], "params")) { params(); }
    else if (!strcmp(argv[0], "stop"))   { bridge_stop("stop command"); }
    else if (!strcmp(argv[0], "start"))  { start_openloop(); }
    else if (!strcmp(argv[0], "raw") && (argc == 4)) { start_raw(ARGF(1), ARGF(2), ARGF(3)); printf("ready\r\n"); }
    else if (!strcmp(argv[0], "rl"))     { start_rl(); printf("ready\r\n"); }
    else if (!strcmp(argv[0], "trace"))  { trace_dump((argc > 1) ? (uint32_t)ARGF(1) : 200u); }
    else if (!strcmp(argv[0], "if"))     { start_foc(F_IF); }
    else if (!strcmp(argv[0], "sl"))     { start_foc(F_SL); }
    else if (!strcmp(argv[0], "mode") && (argc == 2))
    {
        Mod m;
        if (s_state != S_IDLE)           { printf("stop first\r\n"); }
        else if (parse_mod(argv[1], &m)) { printf("unknown mode\r\n"); }
        else                             { s_mod = m; printf("mode %s\r\n", k_mod[m]); }
    }
    else if (!strcmp(argv[0], "amp"))    { p_amp = clampf(ARGF(1), 0.0f, 60.0f); }
    else if (!strcmp(argv[0], "f"))      { p_f = clampf(ARGF(1), 0.5f, 400.0f); }
    else if (!strcmp(argv[0], "ilim"))   { p_ilim = clampf(ARGF(1), 0.2f, 4.5f); }
    else if (!strcmp(argv[0], "vbus"))   { p_vbus = clampf(ARGF(1), 4.5f, 30.0f); }
    else if (!strcmp(argv[0], "fstart")) { p_fstart = clampf(ARGF(1), 0.5f, 50.0f); }
    else if (!strcmp(argv[0], "align"))  { p_align = (uint32_t)clampf(ARGF(1), 0.0f, 5000.0f); }
    else if (!strcmp(argv[0], "accel"))  { p_acc0 = ARGF(1); p_acc1 = ARGF(2); }
    else if (!strcmp(argv[0], "log"))    { p_log = (uint32_t)clampf(ARGF(1), 50.0f, 10000.0f); }
    else if (!strcmp(argv[0], "set") && (argc == 3))
    {
        const char *k = argv[1];
        float v = ARGF(2);
        if      (!strcmp(k, "R"))   { p_R = v; }
        else if (!strcmp(k, "L"))   { p_L = v * 1e-6f; }
        else if (!strcmp(k, "lam")) { p_lam = v; }
        else if (!strcmp(k, "bw"))  { p_bw = clampf(v, 50.0f, 2000.0f); }
        else if (!strcmp(k, "ifa")) { p_ifa = clampf(v, 0.1f, 4.0f); }
        else if (!strcmp(k, "fsw")) { p_fsw = clampf(v, 5.0f, 300.0f); }
        else if (!strcmp(k, "kps")) { p_kps = v; }
        else if (!strcmp(k, "kis")) { p_kis = v; }
        else if (!strcmp(k, "dtc")) { p_dtc = (v != 0.0f) ? 1u : 0u; }
        else if (!strcmp(k, "pll")) { p_pll = clampf(v, 5.0f, 200.0f); }
        else if (!strcmp(k, "idsl")) { p_idsl = clampf(v, -3.0f, 0.0f); }
        else if (!strcmp(k, "vrec")) { p_vrec = (uint8_t)clampf(v, 0.0f, 2.0f); }
        else if (!strcmp(k, "trdec")) { p_trdec = (uint32_t)clampf(v, 1.0f, 100.0f); }
        else if (!strcmp(k, "focmod"))
        {
            Mod m;
            if (parse_mod(argv[2], &m) || (m == M_6STEP)) { printf("focmod sine|svpwm|dpwm\r\n"); }
            else { p_focmod = m; }
        }
        else { printf("unknown parameter\r\n"); }
    }
    else { printf("? (help)\r\n"); }
    if (s_state == S_IDLE) { printf("ok\r\n"); }
#undef ARGF
}

int main(void)
{
    char line[64];
    uint32_t t_log;
    State prev = S_IDLE;

    Board_Init();
    printf("\r\n=== test_08_drive_lab ===\r\n");

    Board_DrvWake();
    if (DRV8316_Init() != 0)
    {
        printf("DRV8316 nFAULT low after wake: check VIN.\r\n");
    }
    Board_InitTim1(PWM_ARR_20KHZ);
    Board_Tim1AdcTrigger();
    Board_InitAdc1();
    s_arr = PWM_ARR_20KHZ;
    TIM1->CR2 |= TIM_CR2_CCPC;
    TIM1->CCMR1 |= TIM_CCMR1_OC1PE | TIM_CCMR1_OC2PE;
    TIM1->CCMR2 |= TIM_CCMR2_OC3PE;
    bridge_stop("init");
    if ((HAL_TIM_Base_Start(&htim1) != HAL_OK) ||
        (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_dma, ADC_BUF_LEN) != HAL_OK))
    {
        printf("TIM1/ADC start failed\r\n");
        Error_Handler();
    }
    Board_DelayMs(10u);
    params();
    help();
    printf("ready\r\n");
    t_log = Board_Millis();

    for (;;)
    {
        State now = s_state;

        if (Board_StopRequested() != 0u)
        {
            bridge_stop("ESC");
            Board_ClearStop();
        }
        if (Usb_GetLine(line, sizeof(line)) != 0u)
        {
            command(line);
            t_log = Board_Millis();
        }
        if ((now != S_IDLE) && (now != S_RAW) && (now != S_RL) &&
            ((Board_Millis() - t_log) >= p_log))
        {
            t_log += p_log;
            log_line(p_log);
        }
        if ((prev != S_IDLE) && (now == S_IDLE) && (prev != S_RAW) && (prev != S_RL))
        {
            printf("RESULT %s: stopped (%s), max f %.1f Hz, max |I| sample %.2f A\r\n",
                   (prev == S_FOC) ? ((s_kind == F_IF) ? "I/f" : "sensorless") : k_mod[s_mod],
                   s_stop_reason ? s_stop_reason : "?", (double)s_fmax_seen, (double)s_imax_seen);
            printf("ready\r\n");
        }
        if ((prev == S_RAW || prev == S_RL) && (now == S_IDLE))
        {
            printf("ready\r\n");
        }
        prev = now;
        Board_DelayMs(1u);
    }
}
