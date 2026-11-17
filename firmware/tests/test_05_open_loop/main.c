/**
 * @file    main.c
 * @brief   test_05_open_loop — open-loop 6-step drive with current limiter.
 *
 * Port of the bring-up firmware that was verified on this board
 * (varun_foc six_step_v1, 2026-11-17): a drone motor at 9 V ran up to
 * 200 Hz electrical with a 2 A limit.
 *
 * Why 6-step and not V/f sine: with DRV8316 pin 35 (OCP/SR) floating, ASR/AAR
 * active demagnetisation is on and the bridge passes almost no current when
 * all three phases switch. 6-step always holds one phase's low side on with
 * no edges, which is the case that was measured to work.
 *
 * Full description, measurements and tuning: docs/trapezoidal.md
 *
 * Covers the whole power path without the FOC loop:
 *   - DRV8316T wake, nFAULT, idle = DRVOFF low + INx low
 *   - TIM1 complementary PWM, 1 us dead time, atomic commutation (CCPC/COMG)
 *   - phase mapping A = CH3, B = CH2, C = CH1
 *   - CSA offset check and live phase current (0.3 V/A)
 *
 * Per step each phase is one of:
 *   P  PWM mode 1, CHx + CHxN on: high side PWM, synchronous low side
 *   L  forced inactive, CHx + CHxN on: low side held on (carries the full
 *      winding current all period, so its SOx measures the motor current)
 *   Z  outputs off: INH = INL = 0 (pull-downs) -> Hi-Z
 *
 * Sequence: CSA offsets (bridge off) -> align on step 0 -> commutate from
 * T5_FSTART_HZ, ramping df/dt = T5_ACCEL + T5_ACCEL_PROP * f up to T5_FMAX_HZ.
 * Duty slews to T5_DUTY_PCT but a current limiter decides the real duty.
 * Overcurrent trip -> retry after 3 s. nFAULT -> retry after 1 s.
 *
 * SAFETY: open loop, the motor must be free to spin. Use a current-limited
 * supply. Ctrl-C or ESC on the USB console stops the bridge for good.
 * Every T5_ macro can be overridden: make APP=test_05_open_loop
 * USER_DEFS="-DT5_FMAX_HZ=100.0f -DT5_ILIM_A=1.5f" flash
 */
#include "board.h"
#include "debug.h"

#include <math.h>

/* ---- profile: verified drone-motor settings (9 V, ~0.5 ohm l-l) ---------- */
#ifndef T5_DUTY_PCT
#define T5_DUTY_PCT         40.0f   /* max duty; the limiter applies less     */
#endif
#ifndef T5_FMAX_HZ
#define T5_FMAX_HZ          200.0f  /* electrical                             */
#endif
#ifndef T5_FSTART_HZ
#define T5_FSTART_HZ        4.0f    /* electrical, right after alignment      */
#endif
#ifndef T5_ACCEL
#define T5_ACCEL            5.0f    /* Hz/s                                   */
#endif
#ifndef T5_ACCEL_PROP
#define T5_ACCEL_PROP       0.1f    /* 1/s; 4 -> 200 Hz in about 15 s         */
#endif
#ifndef T5_ALIGN_MS
#define T5_ALIGN_MS         500u
#endif
#ifndef T5_ILIM_A
#define T5_ILIM_A           2.0f    /* phase current limit                    */
#endif
#ifndef T5_POLE_PAIRS
#define T5_POLE_PAIRS       7u      /* for the rpm column only                */
#endif

#define I_TRIP_MAX          5.3f    /* ADC saturates ~5.4 A; DRV is 8 A peak  */
#define TRIP_MS             20u
#define DUTY_NOCSA_MAX      8.0f    /* duty cap if the CSA is unusable        */
#define DUTY_SLEW_PCT_MS    0.05f
#define CAP_DOWN_PCT_MS     0.1f
#define CAP_UP_PCT_MS       0.02f
#define CAP_START_PCT       2.0f
#define COMM_TICK_HZ        100000u /* TIM6 tick = 10 us                      */
#define OC_RETRY_MS         3000u
#define FAULT_RETRY_MS      1000u

/* ---- 6-step table ------------------------------------------------------- */
enum { P = 0, L = 1, Z = 2 };

/* {A, B, C}. Forward = step + 1. */
static const uint8_t k_steps[6][3] = {
    { P, L, Z },    /* 0  A+ B- */
    { P, Z, L },    /* 1  A+ C- */
    { Z, P, L },    /* 2  B+ C- */
    { L, P, Z },    /* 3  B+ A- */
    { L, Z, P },    /* 4  C+ A- */
    { Z, L, P },    /* 5  C+ B- */
};
static const uint8_t k_phase_ch[3] = { 3u, 2u, 1u };   /* A = CH3, B = CH2, C = CH1 */

#define OCM_FORCE_LOW       4u      /* OCxREF forced low -> CHx = 0, CHxN = 1 */
#define OCM_PWM1            6u

typedef enum { ST_IDLE = 0, ST_ALIGN, ST_RUN } RunState;

/* ---- shared with the ISRs ----------------------------------------------- */
static volatile uint16_t g_dma[ADC_BUF_LEN];
static volatile RunState s_state;
static volatile uint8_t  s_step;
static volatile uint32_t s_steps;
static volatile uint8_t  s_tripped;     /* 1 = overcurrent, 2 = nFAULT */
static volatile float    s_freq;
static volatile float    s_duty_cur;
static volatile float    s_duty_applied;
static volatile float    s_cap;
static volatile float    s_i_filt;
static volatile float    s_i_peak;
static volatile uint32_t s_state_ms;
static float             s_off[3];      /* zero-current SOx, ADC counts */
static uint8_t           s_csa_ok;
static uint8_t           s_ph_ok[3];    /* per-phase CSA usable */
static uint32_t          s_arr;

/* ---- TIM1 output control ------------------------------------------------ */
static void set_ocm(uint32_t ch, uint32_t mode)
{
    /* OCxM is bits [6:4] (+ bit 16) for odd channels, [14:12] (+ bit 24) for
     * even ones. Only modes < 8 are used, so bit 16/24 is cleared. */
    volatile uint32_t *ccmr = (ch <= 2u) ? &TIM1->CCMR1 : &TIM1->CCMR2;
    uint32_t sh  = ((ch & 1u) != 0u) ? 4u : 12u;
    uint32_t msk = (7u << sh) | (1u << (sh + 12u));

    *ccmr = (*ccmr & ~msk) | (mode << sh);
}

/* Preload step s (applied on the next COMG). Called with IRQs off or from
 * the commutation ISR. */
static void load_step(uint8_t s)
{
    uint32_t ccer = TIM1->CCER & ~(TIM_CCER_CC1E | TIM_CCER_CC1NE |
                                   TIM_CCER_CC2E | TIM_CCER_CC2NE |
                                   TIM_CCER_CC3E | TIM_CCER_CC3NE);

    for (uint32_t p = 0u; p < 3u; p++)
    {
        uint32_t ch = k_phase_ch[p];
        uint32_t en = (1u << (4u * (ch - 1u))) | (1u << (4u * (ch - 1u) + 2u));

        switch (k_steps[s][p])
        {
        case P:  set_ocm(ch, OCM_PWM1);      ccer |= en; break;
        case L:  set_ocm(ch, OCM_FORCE_LOW); ccer |= en; break;
        default: break;                                  /* Z: off */
        }
    }
    TIM1->CCER = ccer;
    TIM1->EGR  = TIM_EGR_COMG;
}

static void set_duty(float pct)
{
    uint32_t ccr = (uint32_t)(pct * 0.01f * (float)(s_arr + 1u));

    TIM1->CCR1 = ccr;
    TIM1->CCR2 = ccr;
    TIM1->CCR3 = ccr;
}

static uint32_t step_period_ticks(float f)
{
    float t = (float)COMM_TICK_HZ / (6.0f * f);

    if (t > 65535.0f) { t = 65535.0f; }
    if (t < 2.0f)     { t = 2.0f; }
    return (uint32_t)t;
}

static void motor_stop(void)
{
    __disable_irq();
    TIM6->CR1 &= ~TIM_CR1_CEN;
    Board_BridgeOff();                  /* MOE = 0, DRVOFF low */
    TIM1->CCER &= ~(TIM_CCER_CC1E | TIM_CCER_CC1NE | TIM_CCER_CC2E |
                    TIM_CCER_CC2NE | TIM_CCER_CC3E | TIM_CCER_CC3NE);
    TIM1->EGR = TIM_EGR_COMG;
    set_duty(0.0f);
    s_duty_cur = 0.0f;
    s_duty_applied = 0.0f;
    s_freq = 0.0f;
    s_state = ST_IDLE;
    __enable_irq();
}

/* Zero-current offsets with the bridge off. Returns 1 if usable: at least two
 * phases good. In six-step the held-low (L) and PWM (P) phase carry the same
 * current, so one bad SOx (e.g. an open pin 38 = SOC) can be covered by the
 * other phase of the conducting pair. */
static uint8_t csa_calibrate(void)
{
    float    sum[3] = { 0.0f, 0.0f, 0.0f };
    uint8_t  nok = 0u;
    const float lsb = 3.3f / 4095.0f;

    for (uint32_t k = 0u; k < 64u; k++)
    {
        Board_DelayUs(100u);
        for (uint32_t i = 0u; i < 3u; i++)
        {
            sum[i] += (float)g_dma[i];
        }
    }
    for (uint32_t i = 0u; i < 3u; i++)
    {
        float v;

        s_off[i] = sum[i] / 64.0f;
        v = s_off[i] * lsb;
        s_ph_ok[i] = ((v >= DRV8316_CSA_OFF_MIN_V) && (v <= DRV8316_CSA_OFF_MAX_V)) ? 1u : 0u;
        nok += s_ph_ok[i];
    }
    DBG_Printf("CSA offsets A %.3f%s B %.3f%s C %.3f%s V, %.2f V/A -> %s\r\n",
               (double)(s_off[0] * lsb), s_ph_ok[0] ? "" : "(BAD)",
               (double)(s_off[1] * lsb), s_ph_ok[1] ? "" : "(BAD)",
               (double)(s_off[2] * lsb), s_ph_ok[2] ? "" : "(BAD)",
               (double)DRV8316_CSA_V_PER_A,
               (nok >= 2u) ? ((nok == 3u) ? "OK" : "OK (bad phase covered by the other phase of each pair)")
                           : "NOT USABLE (duty capped at 8 %)");
    return (nok >= 2u) ? 1u : 0u;
}

/* Returns 0 if the bridge was started. */
static int motor_start(void)
{
    if ((Board_DrvFault() != 0u) && (DRV8316_ClearFault() != 0))
    {
        return -1;
    }

    s_csa_ok = csa_calibrate();         /* bridge is off here */

    __disable_irq();
    s_cap      = CAP_START_PCT;
    s_i_filt   = 0.0f;
    s_i_peak   = 0.0f;
    s_duty_cur = 0.0f;
    s_tripped  = 0u;
    s_step     = 0u;
    set_duty(0.0f);
    load_step(0u);
    Board_DrvOutputEnable();            /* DRVOFF low (already) */
    TIM1->BDTR |= TIM_BDTR_MOE;
    s_state_ms = 0u;
    s_state    = ST_ALIGN;
    __enable_irq();
    return 0;
}

/* ---- commutation: TIM6 ------------------------------------------------- */
void TIM6_DAC_IRQHandler(void)
{
    if ((TIM6->SR & TIM_SR_UIF) == 0u)
    {
        return;
    }
    TIM6->SR = ~TIM_SR_UIF;

    if (s_state != ST_RUN)
    {
        return;
    }
    s_step = (uint8_t)((s_step + 1u) % 6u);
    load_step(s_step);
    s_steps++;
}

static void comm_timer_start(void)
{
    s_freq = T5_FSTART_HZ;
    TIM6->CNT = 0u;
    TIM6->ARR = step_period_ticks(s_freq) - 1u;
    TIM6->EGR = TIM_EGR_UG;
    TIM6->SR  = 0u;
    TIM6->CR1 |= TIM_CR1_CEN;
}

static void comm_timer_init(void)
{
    __HAL_RCC_TIM6_CLK_ENABLE();
    TIM6->PSC  = (SystemCoreClock / COMM_TICK_HZ) - 1u;
    TIM6->ARR  = 0xFFFFu;
    TIM6->CR1  = TIM_CR1_ARPE;
    TIM6->EGR  = TIM_EGR_UG;
    TIM6->SR   = 0u;
    TIM6->DIER = TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM6_DAC_IRQn, 2u, 0u);
    HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn);
}

/* ---- 20 kHz current sample + 1 kHz control (ADC DMA complete) ----------- */
static float slew(float x, float target, float step)
{
    if (x < target) { x += step; if (x > target) { x = target; } }
    else            { x -= step; if (x < target) { x = target; } }
    return x;
}

static void control_1ms(void)
{
    static uint32_t over_ms;
    static uint32_t fault_ms;
    float target;
    float trip;
    float d;

    if (s_state == ST_IDLE)
    {
        return;
    }
    s_state_ms++;

    /* nFAULT, debounced 2 ms. */
    fault_ms = (Board_DrvFault() != 0u) ? (fault_ms + 1u) : 0u;
    if (fault_ms >= 2u)
    {
        fault_ms = 0u;
        motor_stop();
        s_tripped = 2u;
        return;
    }

    /* Duty slew and current limiter. */
    target = T5_DUTY_PCT;
    if ((s_csa_ok == 0u) && (target > DUTY_NOCSA_MAX))
    {
        target = DUTY_NOCSA_MAX;
    }
    s_duty_cur = slew(s_duty_cur, target, DUTY_SLEW_PCT_MS);

    if (s_csa_ok != 0u)
    {
        float cap = s_cap;

        if (s_i_filt > T5_ILIM_A)
        {
            if (s_duty_applied < cap) { cap = s_duty_applied; }
            cap -= CAP_DOWN_PCT_MS;
            if (cap < 0.0f) { cap = 0.0f; }
        }
        else
        {
            cap += CAP_UP_PCT_MS;
            if (cap > s_duty_applied + 2.0f) { cap = s_duty_applied + 2.0f; }
        }
        s_cap = cap;

        trip = 2.0f * T5_ILIM_A;
        if (trip > I_TRIP_MAX) { trip = I_TRIP_MAX; }
        over_ms = (s_i_filt > trip) ? (over_ms + 1u) : 0u;
        if (over_ms >= TRIP_MS)
        {
            over_ms = 0u;
            motor_stop();
            s_tripped = 1u;
            return;
        }
    }

    d = s_duty_cur;
    if ((s_csa_ok != 0u) && (d > s_cap)) { d = s_cap; }
    s_duty_applied = d;
    set_duty(d);

    /* Align, then ramp the commutation frequency. */
    if (s_state == ST_ALIGN)
    {
        if (s_state_ms >= T5_ALIGN_MS)
        {
            s_state = ST_RUN;
            comm_timer_start();
        }
    }
    else
    {
        float f = slew(s_freq, T5_FMAX_HZ, (T5_ACCEL + T5_ACCEL_PROP * s_freq) * 0.001f);

        s_freq = f;
        TIM6->ARR = step_period_ticks(f) - 1u;  /* preloaded: next step */
    }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    static uint32_t div;

    if (hadc->Instance != ADC1)
    {
        return;
    }

    if ((s_state != ST_IDLE) && (s_csa_ok != 0u))
    {
        /* The L phase's low side is on all period, so its SOx is the full
         * winding current. Sampled at the counter peak (TIM1 OC4REF). */
        uint8_t lp = 0u, pp = 0u, mp;
        float   i;

        for (uint8_t p = 0u; p < 3u; p++)
        {
            if (k_steps[s_step][p] == L) { lp = p; }
            if (k_steps[s_step][p] == P) { pp = p; }
        }
        /* L phase normally; the P phase (its low side is on at the counter
         * peak) if the L phase's CSA is bad */
        mp = (s_ph_ok[lp] != 0u) ? lp : pp;
        i = fabsf(((float)g_dma[mp] - s_off[mp]) * (3.3f / 4095.0f) / DRV8316_CSA_V_PER_A);
        s_i_filt += 0.01f * (i - s_i_filt);     /* tau ~ 5 ms */
        if (i > s_i_peak) { s_i_peak = i; }
    }

    if (++div >= 20u)                           /* 20 kHz -> 1 kHz */
    {
        div = 0u;
        control_1ms();
    }
}

/* ------------------------------------------------------------------------ */
int main(void)
{
    uint32_t t_log;
    uint32_t steps_prev = 0u;
    uint32_t retry_at = 0u;

    Board_Init();

    DBG_Puts("\r\n\r\n=== test_05_open_loop (6-step, current limited) ===\r\n");
    DBG_Printf("duty %.0f %%  ilim %.1f A  f %.0f -> %.0f Hz el.  align %u ms\r\n",
               (double)T5_DUTY_PCT, (double)T5_ILIM_A, (double)T5_FSTART_HZ,
               (double)T5_FMAX_HZ, (unsigned)T5_ALIGN_MS);

    Board_DrvSleep();
    Board_DelayMs(2u);
    Board_DrvWake();
    if (DRV8316_Init() != 0)
    {
        DBG_Puts("DRV8316 nFAULT low after wake: check VIN on J1. Will keep retrying.\r\n");
    }

    /* PWM + current sampling. TIM1 counts (ADC trigger) but MOE stays 0. */
    Board_InitTim1(PWM_ARR_20KHZ);
    Board_Tim1AdcTrigger();
    Board_InitAdc1();
    s_arr = PWM_ARR_20KHZ;
    TIM1->CR2 |= TIM_CR2_CCPC;          /* CCxE/CCxNE/OCxM applied on COMG */
    TIM1->CCMR1 |= TIM_CCMR1_OC1PE | TIM_CCMR1_OC2PE;
    TIM1->CCMR2 |= TIM_CCMR2_OC3PE;
    motor_stop();
    comm_timer_init();

    if ((HAL_TIM_Base_Start(&htim1) != HAL_OK) ||
        (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_dma, ADC_BUF_LEN) != HAL_OK))
    {
        DBG_Puts("TIM1/ADC start failed.\r\n");
        Error_Handler();
    }
    Board_DelayMs(10u);

    if (motor_start() != 0)
    {
        DBG_Puts("Start refused: nFAULT low and would not clear. Retrying every 1 s.\r\n");
        s_tripped = 2u;
    }
    retry_at = Board_Millis();

    DBG_Puts("\r\n   t_s  state  rpm    f_Hz  steps/s  duty%  I_A  (peak)  nFAULT\r\n");
    t_log = Board_Millis();

    for (;;)
    {
        uint32_t now = Board_Millis();

        if (Board_StopRequested() != 0u)
        {
            motor_stop();
            DBG_Puts("Stopped from the console. Reflash or power-cycle to run again.\r\n");
            for (;;) { Board_DelayMs(1000u); }
        }

        /* Auto-retry after an overcurrent trip or a driver fault. */
        if ((s_state == ST_IDLE) && (s_tripped != 0u))
        {
            uint32_t wait = (s_tripped == 1u) ? OC_RETRY_MS : FAULT_RETRY_MS;
            static uint8_t reported;

            if (reported != s_tripped)
            {
                DBG_Printf("STOP: %s. Retrying in %lu ms.\r\n",
                           (s_tripped == 1u) ? "overcurrent" : "nFAULT",
                           (unsigned long)wait);
                reported  = s_tripped;
                retry_at  = now;
            }
            if ((now - retry_at) >= wait)
            {
                reported = 0u;
                if (motor_start() != 0)
                {
                    s_tripped = 2u;
                    retry_at  = now;
                    reported  = 2u;     /* stay quiet while it keeps failing */
                }
            }
        }

        if ((now - t_log) >= 1000u)
        {
            uint32_t steps = s_steps;
            float    sps   = (float)(steps - steps_prev) * 1000.0f / (float)(now - t_log);

            steps_prev = steps;
            t_log      = now;
            DBG_Printf("%6.1f  %-5s %6.0f  %6.1f  %7.0f  %5.1f  %4.2f (%4.2f)  %s\r\n",
                       (double)now * 0.001,
                       (s_state == ST_RUN) ? "RUN" : (s_state == ST_ALIGN) ? "ALIGN" : "IDLE",
                       (double)(s_freq * 60.0f / (float)T5_POLE_PAIRS),
                       (double)s_freq, (double)sps,
                       (double)s_duty_applied, (double)s_i_filt, (double)s_i_peak,
                       Board_DrvFault() ? "LOW" : "high");
            s_i_peak = 0.0f;
        }

        Board_DelayMs(5u);
    }
}
