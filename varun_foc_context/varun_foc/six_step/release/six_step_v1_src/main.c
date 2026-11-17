// Open-loop 6-step (trapezoidal) commutation for the BLDC board
// (STM32G431CBT6 + DRV8316T in 6x PWM mode).
//
// The board has no Hall sensors and no phase-voltage dividers, so back-EMF
// zero-crossings cannot be detected: commutation is forced at a commanded
// electrical frequency (like a stepper), after an initial rotor alignment.
//
// Control is over the USB CDC serial port (minicom / picocom, any baud):
//   start | stop | manual | auto | duty <%> | freq <Hz> | rpm <n> | pp <n>
//   accel <Hz/s> | align <ms> | dir | watch | log | vin <V> | status | help | dfu
//   Manual mode keys (no Enter needed): n or + next step, b or - previous,
//   0..5 jump to a step.   Ctrl-C or ESC = immediate stop, any mode.
//
// Pin map (README section 7.2):
//   Phase A: PA10 TIM1_CH3 (INHA) / PB15 TIM1_CH3N (INLA)
//   Phase B: PA9  TIM1_CH2 (INHB) / PB14 TIM1_CH2N (INLB)
//   Phase C: PA8  TIM1_CH1 (INHC) / PB13 TIM1_CH1N (INLC)
//   PA6 nFAULT (in, ext. pull-up)  PA7 DRVOFF (out, high = all FETs off)
//   DRVOFF is kept LOW even when idle: per DRV8316 datasheet 8.4.2, DRVOFF high
//   pulls nFAULT low. Idle safety comes from all six INx being low (= Hi-Z).
//   PC13 nSLEEP (out, high = awake, 20-40 us low pulse clears faults)
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stm32g4xx.h"
#include "tusb.h"

// ---------------------------------------------------------------- tunables
#define PWM_FREQ_HZ       20000u
#define DEADTIME_TICKS    16u       // 16 / 16 MHz = 1.0 us (DRV8316 SLEW 125 V/us)
#define DUTY_MAX_PCT      50.0f     // hard cap on commanded duty
#define DUTY_SLEW_PCT_MS  0.05f     // duty ramp: 0.05 %/ms = 5 % per 100 ms
#define COMM_TICK_HZ      100000u   // TIM6 tick = 10 us
#define FREQ_MIN_HZ       0.5f      // electrical; 333 ms per step
#define FREQ_MAX_HZ       500.0f
#define FREQ_START_HZ     4.0f      // commutation rate right after alignment

// AUTOSTART=1 (make AUTOSTART=1): commutate from power-up, retry after
// faults, and log every second. Meant for scope checks with no motor.
#ifndef AUTOSTART
#define AUTOSTART 0
#endif
#define AUTO_DUTY_PCT     40.0f     // max duty; the current limiter applies less
#define AUTO_FREQ_HZ      200.0f    // electrical; top speed reached on the 9 V test motor

// Current sensing (DRV8316 CSA, GAIN pin open = 0.3 V/A, bias = VREF/2).
// The phase whose low side is held on carries the full winding current, so
// its SOx gives the motor current. A limiter trims duty to keep it under
// i_limit; above 2 x i_limit (max 4 A) for 20 ms the bridge is shut off.
#define CSA_GAIN_V_PER_A  0.3f
#define I_LIMIT_DEFAULT   2.0f      // A (trip at 4 A)
#define DUTY_NOCSA_MAX    8.0f      // duty cap if current sensing is unusable
#define OC_RETRY_MS       3000u
#define LOG_PERIOD_MS     1000u
#define FAULT_RETRY_MS    1000u
#define WATCH_MIN_GAP_MS  50u       // watch mode prints at most 20 lines/s

#define BOOTLOADER_MAGIC  0xB007DF11u
#define SYSMEM_BASE       0x1FFF0000u

// ---------------------------------------------------------------- pins
#define PIN_FAULT  6   // PA6
#define PIN_DRVOFF 7   // PA7
#define PIN_SLEEP  13  // PC13

static volatile uint32_t boot_request __attribute__((section(".noinit")));
static volatile uint32_t ms_ticks;

// ---------------------------------------------------------------- motor state
typedef enum { M_IDLE, M_ALIGN, M_RUN, M_MANUAL, M_PHASE } mstate_t;
static const char *const state_names[] = {"IDLE", "ALIGN", "RUN", "MANUAL", "PHASE"};

static volatile mstate_t state = M_IDLE;
static volatile mstate_t fault_state;        // state we were in when nFAULT hit
static volatile uint32_t state_ms;
static volatile uint8_t  step;
static volatile uint32_t step_count;
static volatile bool     dir_fwd = true;

static volatile float duty_target = 8.0f;    // %
static volatile float duty_cur;              // %
static volatile float freq_target = 20.0f;   // Hz electrical
static volatile float freq_cur;              // Hz electrical
// Ramp: d(freq)/dt = accel + ACCEL_PER_HZ * freq. Slow at standstill, picking
// up pace with speed: 4 -> 200 Hz takes ~15 s with the defaults.
#define ACCEL_PER_HZ      0.1f      // 1/s
static volatile float accel = 5.0f;          // Hz/s at standstill
static volatile float i_limit = I_LIMIT_DEFAULT;   // A
static volatile float i_filt, i_peak;        // A (filtered, peak since report)
static volatile float duty_cap = 50.0f;      // % allowed by the current limiter
static volatile float duty_applied;          // % actually on the timer
static float cs_off[3];                      // V, zero-current SOx (calibrated)
static float vdda_cal = 3.3f;
static bool csa_ok;
static volatile uint32_t load_seq;           // bumps on every commutation
static const uint8_t *volatile active_modes; // P/L/Z of the live bridge state
static volatile uint32_t align_ms = 500;
static volatile uint32_t pole_pairs = 7;
static float vin_volts = 12.0f;              // display only (no VM sensing)

#define EV_FAULT    (1u << 0)
#define EV_RUNNING  (1u << 1)
#define EV_AT_SPEED (1u << 2)
#define EV_OVERCURRENT (1u << 3)
static volatile uint32_t events;

static uint16_t pwm_arr;

// ---------------------------------------------------------------- 6-step table
// Per step, each phase is: P = PWM on high side (complementary, sync. rect.),
// L = low side held on, Z = floating (both FETs off).
enum { Z = 0, P = 1, L = 2 };
static const uint8_t steps[6][3] = {   // {A, B, C}
  {P, L, Z},   // 0: A+ B-
  {P, Z, L},   // 1: A+ C-
  {Z, P, L},   // 2: B+ C-
  {L, P, Z},   // 3: B+ A-
  {L, Z, P},   // 4: C+ A-
  {Z, L, P},   // 5: C+ B-
};
static const char *const step_names[6] = {"A+ B-", "A+ C-", "B+ C-", "B+ A-", "C+ A-", "C+ B-"};
static const uint8_t phase_ch[3] = {3, 2, 1};   // A->CH3, B->CH2, C->CH1

#define OCM_FORCE_LOW 4u   // OCxREF forced low -> CHx = 0, CHxN = 1
#define OCM_PWM1      6u

// Writes the preloaded CCMR/CCER bits for step s, then fires a COM event so
// all three phases switch on the same clock edge.
static void load_modes(const uint8_t m3[3]) {
  uint32_t ccmr1 = TIM_CCMR1_OC1PE | TIM_CCMR1_OC2PE;
  uint32_t ccmr2 = TIM_CCMR2_OC3PE;
  uint32_t ccer = 0;

  for (int ph = 0; ph < 3; ph++) {
    uint8_t ch = phase_ch[ph];
    uint8_t m = m3[ph];
    uint32_t ocm = (m == P) ? OCM_PWM1 : OCM_FORCE_LOW;
    if (ch == 1)      ccmr1 |= ocm << TIM_CCMR1_OC1M_Pos;
    else if (ch == 2) ccmr1 |= ocm << TIM_CCMR1_OC2M_Pos;
    else              ccmr2 |= ocm << TIM_CCMR2_OC3M_Pos;
    // Floating phase: CCxE = CCxNE = 0 -> pins not driven, GPIO pull-downs
    // plus the DRV8316's own input pull-downs give INH = INL = 0 = Hi-Z.
    if (m != Z) ccer |= (TIM_CCER_CC1E | TIM_CCER_CC1NE) << (4 * (ch - 1));
  }
  TIM1->CCMR1 = ccmr1;
  TIM1->CCMR2 = ccmr2;
  TIM1->CCER = ccer;
  TIM1->EGR = TIM_EGR_COMG;
  active_modes = m3;
  load_seq++;
}

static void load_step(uint8_t s) { load_modes(steps[s]); }

static void set_duty(float pct) {
  uint32_t ccr = (uint32_t)(pct * pwm_arr / 100.0f);
  TIM1->CCR1 = ccr;
  TIM1->CCR2 = ccr;
  TIM1->CCR3 = ccr;
}

static uint32_t step_period_ticks(float f) {
  if (f < FREQ_MIN_HZ) f = FREQ_MIN_HZ;
  return (uint32_t)((float)COMM_TICK_HZ / (6.0f * f));
}

static bool fault_active(void) { return !(GPIOA->IDR & (1u << PIN_FAULT)); }

static void busy_wait_us(uint32_t us) {
  uint32_t n = us * (SystemCoreClock / 4000000u);  // ~4 cycles per iteration
  while (n--) __NOP();
}

static void delay_ms_usb(uint32_t ms) {
  uint32_t t = ms_ticks;
  while ((ms_ticks - t) < ms) tud_task();
}

// ---------------------------------------------------------------- start/stop
static void motor_stop(void) {
  __disable_irq();
  TIM6->CR1 &= ~TIM_CR1_CEN;
  TIM1->BDTR &= ~TIM_BDTR_MOE;            // all INx to idle (low) = bridge Hi-Z
  GPIOA->BRR = 1u << PIN_DRVOFF;          // DRVOFF high would fake an nFAULT
  TIM1->CCER = 0;
  TIM1->EGR = TIM_EGR_COMG;
  set_duty(0);
  duty_cur = 0;
  duty_applied = 0;
  freq_cur = 0;
  state = M_IDLE;
  __enable_irq();
}

// Pulses nSLEEP if nFAULT is low. Returns NULL if the driver is fault-free.
static const char *clear_fault(void) {
  if (!fault_active()) return NULL;
  GPIOC->BRR = 1u << PIN_SLEEP;
  busy_wait_us(30);                       // tRST = 20..40 us
  GPIOC->BSRR = 1u << PIN_SLEEP;
  delay_ms_usb(2);
  return fault_active() ? "nFAULT is low and would not clear" : NULL;
}

// Enables the bridge on step s with duty ramping up from 0. IRQs must be off.
static void csa_calibrate(void);

static void limiter_reset(void) {
  duty_cap = 2.0f;                        // grows with the applied duty
  i_filt = 0;
  i_peak = 0;
}

static void driver_on(uint8_t s) {
  limiter_reset();
  duty_cur = 0;
  set_duty(0);
  step = s;
  load_step(s);
  GPIOA->BRR = 1u << PIN_DRVOFF;
  TIM1->BDTR |= TIM_BDTR_MOE;
  state_ms = 0;
}

// Starts the commutation timer from FREQ_START_HZ (or lower target).
// Called with IRQs off or from SysTick.
static void comm_timer_start(void) {
  freq_cur = freq_target < FREQ_START_HZ ? freq_target : FREQ_START_HZ;
  TIM6->CNT = 0;
  TIM6->ARR = step_period_ticks(freq_cur) - 1;
  TIM6->EGR = TIM_EGR_UG;                 // load ARR now
  TIM6->SR = 0;
  state = M_RUN;
  TIM6->CR1 |= TIM_CR1_CEN;
}

static const char *motor_start(void) {
  if (state != M_IDLE) return "already running (stop first)";
  const char *err = clear_fault();
  if (err) return err;
  csa_calibrate();                        // bridge off: zero current
  __disable_irq();
  step_count = 0;
  driver_on(0);
  state = M_ALIGN;
  __enable_irq();
  return NULL;
}

static const char *manual_enter(void) {
  if (state == M_MANUAL) return NULL;
  if (state == M_IDLE) {
    const char *err = clear_fault();
    if (err) return err;
    csa_calibrate();
    __disable_irq();
    driver_on(step);
    state = M_MANUAL;
    __enable_irq();
  } else {                                // ALIGN or RUN: freeze on this step
    __disable_irq();
    TIM6->CR1 &= ~TIM_CR1_CEN;
    freq_cur = 0;
    state = M_MANUAL;
    __enable_irq();
  }
  return NULL;
}

static void manual_goto(uint8_t s) {
  __disable_irq();
  step = s;
  load_step(s);
  step_count++;
  __enable_irq();
}

// Phase test: any combination of P/L/Z per phase, held until changed.
static uint8_t pt_modes[3];               // all Z = everything off

static const char *phase_enter(void) {
  if (state == M_PHASE) return NULL;
  bool was_idle = (state == M_IDLE);
  if (was_idle) {
    const char *err = clear_fault();
    if (err) return err;
    csa_calibrate();
  }
  __disable_irq();
  TIM6->CR1 &= ~TIM_CR1_CEN;
  freq_cur = 0;
  memset(pt_modes, Z, sizeof pt_modes);
  load_modes(pt_modes);
  if (was_idle) {
    limiter_reset();
    duty_cur = 0;
    set_duty(0);
    GPIOA->BRR = 1u << PIN_DRVOFF;
    TIM1->BDTR |= TIM_BDTR_MOE;
  }
  state_ms = 0;
  state = M_PHASE;
  __enable_irq();
  return NULL;
}

// ---------------------------------------------------------------- interrupts
void USB_HP_IRQHandler(void)    { tud_int_handler(0); }
void USB_LP_IRQHandler(void)    { tud_int_handler(0); }
void USBWakeUp_IRQHandler(void) { tud_int_handler(0); }

// Commutation timer: one interrupt per step.
void TIM6_DAC_IRQHandler(void) {
  if (!(TIM6->SR & TIM_SR_UIF)) return;
  TIM6->SR = ~TIM_SR_UIF;
  if (state != M_RUN) return;

  step = dir_fwd ? (uint8_t)((step + 1) % 6) : (uint8_t)((step + 5) % 6);
  load_step(step);
  step_count++;
  TIM6->ARR = step_period_ticks(freq_cur) - 1;   // preloaded: applies next step
}

static float slew(float cur, float target, float max_delta) {
  if (target > cur + max_delta) return cur + max_delta;
  if (target < cur - max_delta) return cur - max_delta;
  return target;
}

static uint32_t adc_sum(ADC_TypeDef *a, uint32_t ch, int n);

// Runs every ms while the bridge is on. Returns true on overcurrent trip.
static bool current_sense_tick(void) {
  if (!csa_ok) return false;
  const uint8_t *m = active_modes;
  int lp = -1;
  if (m) for (int ph = 0; ph < 3; ph++) if (m[ph] == L) lp = ph;
  if (lp >= 0) {
    uint32_t seq = load_seq;
    uint32_t sum = adc_sum(ADC1, (uint32_t)lp + 1, 8);   // ~30 us
    if (seq == load_seq) {                // no commutation during the burst
      float v = vdda_cal * (float)sum / (8.0f * 4095.0f);
      float i = (v - cs_off[lp]) / CSA_GAIN_V_PER_A;
      if (i < 0) i = -i;
      i_filt += 0.2f * (i - i_filt);
    }
  } else {
    i_filt *= 0.9f;                       // no return path: no current
  }
  if (i_filt > i_peak) i_peak = i_filt;

  // Over the limit: pull the cap below what is actually applied right away.
  // Under it: let the cap creep up, but never more than 2 % above the applied
  // duty, so there is no wind-up to burn through before it bites again.
  float cap;
  if (i_filt > i_limit) cap = (duty_applied < duty_cap ? duty_applied : duty_cap) - 0.1f;
  else {
    cap = duty_cap + 0.02f;
    if (cap > duty_applied + 2.0f) cap = duty_applied + 2.0f;
  }
  duty_cap = cap < 0 ? 0 : cap > DUTY_MAX_PCT ? DUTY_MAX_PCT : cap;

  static uint8_t oc;
  float trip = 2.0f * i_limit;
  if (trip > 4.0f) trip = 4.0f;
  oc = (i_filt > trip) ? (uint8_t)(oc + 1) : 0;
  if (oc >= 20) { oc = 0; return true; }
  return false;
}

void SysTick_Handler(void) {
  ms_ticks++;
  if (state == M_IDLE) return;

  static uint8_t fault_cnt;
  fault_cnt = fault_active() ? (uint8_t)(fault_cnt + 1) : 0;
  if (fault_cnt >= 2) {                   // 2 ms debounce
    fault_cnt = 0;
    fault_state = state;
    motor_stop();
    events |= EV_FAULT;
    return;
  }

  state_ms++;
  duty_cur = slew(duty_cur, duty_target, DUTY_SLEW_PCT_MS);
  if (current_sense_tick()) {
    fault_state = state;
    motor_stop();
    events |= EV_OVERCURRENT;
    return;
  }
  float d = duty_cur < duty_cap ? duty_cur : duty_cap;
  if (!csa_ok && d > DUTY_NOCSA_MAX) d = DUTY_NOCSA_MAX;
  duty_applied = d;
  set_duty(d);

  if (state == M_ALIGN) {
    if (state_ms >= align_ms) {           // rotor parked on step 0: go
      comm_timer_start();
      events |= EV_RUNNING;
    }
  } else if (state == M_RUN) {
    float prev = freq_cur;
    freq_cur = slew(freq_cur, freq_target, (accel + ACCEL_PER_HZ * freq_cur) / 1000.0f);
    if (prev != freq_target && freq_cur == freq_target) events |= EV_AT_SPEED;
  }
}

// ---------------------------------------------------------------- init
static void gpio_mode(GPIO_TypeDef *g, uint32_t pin, uint32_t mode) {
  g->MODER = (g->MODER & ~(3u << (2 * pin))) | (mode << (2 * pin));
}
static void gpio_af(GPIO_TypeDef *g, uint32_t pin, uint32_t af) {
  uint32_t s = (pin & 7) * 4;
  g->AFR[pin >> 3] = (g->AFR[pin >> 3] & ~(0xFu << s)) | (af << s);
}
static void gpio_pull(GPIO_TypeDef *g, uint32_t pin, uint32_t pupd) {
  g->PUPDR = (g->PUPDR & ~(3u << (2 * pin))) | (pupd << (2 * pin));
}
static void gpio_speed(GPIO_TypeDef *g, uint32_t pin, uint32_t spd) {
  g->OSPEEDR = (g->OSPEEDR & ~(3u << (2 * pin))) | (spd << (2 * pin));
}

static void clock_init(void) {
  RCC->CRRCR |= RCC_CRRCR_HSI48ON;
  while (!(RCC->CRRCR & RCC_CRRCR_HSI48RDY)) {}

  RCC->APB1ENR1 |= RCC_APB1ENR1_CRSEN | RCC_APB1ENR1_USBEN | RCC_APB1ENR1_TIM6EN;
  RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;
  RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN | RCC_AHB2ENR_GPIOBEN | RCC_AHB2ENR_GPIOCEN;
  (void)RCC->AHB2ENR;

  RCC->CCIPR &= ~RCC_CCIPR_CLK48SEL;  // HSI48 feeds USB
  CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC) | CRS_CFGR_SYNCSRC_1;  // sync to USB SOF
  CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

  SystemCoreClockUpdate();            // HSI16: 16 MHz core, APB, timers
  SysTick_Config(SystemCoreClock / 1000);
}

static void driver_io_init(void) {
  // DRVOFF high first (outputs off), then wake the driver.
  GPIOA->BSRR = 1u << PIN_DRVOFF;
  gpio_mode(GPIOA, PIN_DRVOFF, 1);
  GPIOC->BSRR = 1u << PIN_SLEEP;
  gpio_mode(GPIOC, PIN_SLEEP, 1);
  gpio_mode(GPIOA, PIN_FAULT, 0);      // input; R6 pulls it up to 3V3
}

static void pwm_init(void) {
  pwm_arr = (uint16_t)(SystemCoreClock / (2u * PWM_FREQ_HZ));  // centre-aligned

  TIM1->CR1 = 0;
  TIM1->PSC = 0;
  TIM1->ARR = pwm_arr;
  TIM1->RCR = 0;
  TIM1->CCR1 = TIM1->CCR2 = TIM1->CCR3 = 0;
  TIM1->CCMR1 = TIM_CCMR1_OC1PE | TIM_CCMR1_OC2PE |
                (OCM_FORCE_LOW << TIM_CCMR1_OC1M_Pos) | (OCM_FORCE_LOW << TIM_CCMR1_OC2M_Pos);
  TIM1->CCMR2 = TIM_CCMR2_OC3PE | (OCM_FORCE_LOW << TIM_CCMR2_OC3M_Pos);
  TIM1->CCER = 0;
  TIM1->CR2 = TIM_CR2_CCPC;           // CCxE/CCxNE/OCxM preloaded, applied on COMG
  TIM1->BDTR = TIM_BDTR_OSSR | TIM_BDTR_OSSI | DEADTIME_TICKS;   // MOE = 0
  TIM1->CR1 = TIM_CR1_CMS_0 | TIM_CR1_ARPE;                      // centre-aligned 1
  TIM1->EGR = TIM_EGR_UG | TIM_EGR_COMG;
  TIM1->CR1 |= TIM_CR1_CEN;

  // Hand the six pins to TIM1, with pull-downs so undriven pins read as 0.
  static const struct { GPIO_TypeDef *g; uint8_t pin, af; } pins[] = {
    {GPIOA, 8, 6}, {GPIOA, 9, 6}, {GPIOA, 10, 6},     // CH1, CH2, CH3
    {GPIOB, 13, 6}, {GPIOB, 14, 6}, {GPIOB, 15, 4},   // CH1N, CH2N, CH3N
  };
  for (unsigned i = 0; i < sizeof pins / sizeof pins[0]; i++) {
    gpio_pull(pins[i].g, pins[i].pin, 2);
    gpio_speed(pins[i].g, pins[i].pin, 2);
    gpio_af(pins[i].g, pins[i].pin, pins[i].af);
    gpio_mode(pins[i].g, pins[i].pin, 2);
  }

  // Commutation timer.
  TIM6->PSC = SystemCoreClock / COMM_TICK_HZ - 1;
  TIM6->ARR = 0xFFFF;
  TIM6->CR1 = TIM_CR1_ARPE;
  TIM6->EGR = TIM_EGR_UG;
  TIM6->SR = 0;
  TIM6->DIER = TIM_DIER_UIE;
  NVIC_SetPriority(TIM6_DAC_IRQn, 1);
  NVIC_EnableIRQ(TIM6_DAC_IRQn);
  NVIC_SetPriority(SysTick_IRQn, 2);
}

// ---------------------------------------------------------------- bootloader
static void jump_to_bootloader_if_requested(void) {
  if (boot_request != BOOTLOADER_MAGIC) return;
  boot_request = 0;

  RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
  (void)RCC->APB2ENR;
  SYSCFG->MEMRMP = SYSCFG_MEMRMP_MEM_MODE_0;
  SCB->VTOR = SYSMEM_BASE;

  uint32_t sp = *(volatile uint32_t *)SYSMEM_BASE;
  uint32_t pc = *(volatile uint32_t *)(SYSMEM_BASE + 4);
  __set_MSP(sp);
  ((void (*)(void))pc)();
  while (1) {}
}

// ---------------------------------------------------------------- serial I/O
static void out(const char *fmt, ...) {
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  if (n <= 0 || !tud_cdc_connected()) return;
  if (n > (int)sizeof buf - 1) n = sizeof buf - 1;

  const char *p = buf;
  uint32_t t = ms_ticks;
  while (n > 0 && (ms_ticks - t) < 100) {
    uint32_t w = tud_cdc_write(p, (uint32_t)n);
    p += w;
    n -= (int)w;
    tud_cdc_write_flush();
    if (n > 0) tud_task();
  }
}

#define F2(x) (int)(x), (int)(((x) - (int)(x)) * 100)   // x.yy, x >= 0
// printf without float support: x.y with one decimal
#define F1(x) (int)(x), (int)((((x) < 0 ? -(x) : (x)) - (int)((x) < 0 ? -(x) : (x))) * 10)

// ---------------------------------------------------------------- pin read-back
// IDR reads the real pin level even in AF mode. Pin order everywhere below:
// INHA INLA INHB INLB INHC INLC.

// One burst of n samples (~1 us each at 16 MHz). Adds high counts to hi[].
static void sample_pins(uint32_t n, uint32_t hi[6]) {
  for (uint32_t i = 0; i < n; i++) {
    uint32_t a = GPIOA->IDR, b = GPIOB->IDR;
    hi[0] += (a >> 10) & 1;  hi[1] += (b >> 15) & 1;   // A: PA10 / PB15
    hi[2] += (a >> 9) & 1;   hi[3] += (b >> 14) & 1;   // B: PA9  / PB14
    hi[4] += (a >> 8) & 1;   hi[5] += (b >> 13) & 1;   // C: PA8  / PB13
  }
}

// Expected % high per pin for step s at duty d (ignoring dead time).
static int expected_pct_m(const uint8_t m3[3], int pin, float d) {
  uint8_t m = m3[pin / 2];
  bool low_side = pin & 1;
  if (m == Z) return 0;
  if (m == L) return low_side ? 100 : 0;
  return low_side ? (int)(100 - d) : (int)d;
}
static int expected_pct(uint8_t s, int pin, float d) { return expected_pct_m(steps[s], pin, d); }

static bool pct_ok(int got, int want) { return got >= want - 8 && got <= want + 8; }

// Background monitor for RUN mode: per-step accumulation, reported by `log`.
static uint32_t mon_hi[6][6], mon_n[6];

static void pin_monitor_sample(void) {
  if (state != M_RUN) return;
  uint8_t s = step;
  uint32_t hi[6] = {0};
  sample_pins(256, hi);                     // ~250 us = ~5 PWM periods
  if (step != s || state != M_RUN) return;  // a commutation landed mid-burst
  for (int p = 0; p < 6; p++) mon_hi[s][p] += hi[p];
  mon_n[s] += 256;
}

static void pin_monitor_report(void) {
  float d = duty_applied;
  int bad = 0;
  out("  step        INHA INLA INHB INLB INHC INLC   (%% high; expect)\r\n");
  for (int s = 0; s < 6; s++) {
    if (!mon_n[s]) { out("  %d %s   (no samples)\r\n", s, step_names[s]); continue; }
    char row[96], exp[48];
    int k = snprintf(row, sizeof row, "  %d %s  ", s, step_names[s]);
    int e = 0;
    for (int p = 0; p < 6; p++) {
      int pct = (int)((mon_hi[s][p] * 100 + mon_n[s] / 2) / mon_n[s]);
      int x = expected_pct((uint8_t)s, p, d);
      if (!pct_ok(pct, x)) bad++;
      k += snprintf(row + k, sizeof row - k, " %4d", pct);
      e += snprintf(exp + e, sizeof exp - e, "%s%d", p ? "/" : "", x);
    }
    out("%s   %s\r\n", row, exp);
  }
  out("  pin check: %s\r\n", bad ? "MISMATCH" : "OK, all pins match the 6-step table");
  memset(mon_hi, 0, sizeof mon_hi);
  memset(mon_n, 0, sizeof mon_n);
}

// ---------------------------------------------------------------- step display
static const char *phase_state_str(uint8_t m) {
  return m == P ? "PWM (HS)" : m == L ? "LOW (LS on)" : "OFF (Hi-Z)";
}

// One compact line, used by `watch` in auto mode.
static void print_step_compact(uint8_t s, uint32_t skipped) {
  out("step %d [%s]  A=%-11s B=%-11s C=%-11s duty %d.%d%%  %d.%d Hz",
      s, step_names[s], phase_state_str(steps[s][0]), phase_state_str(steps[s][1]),
      phase_state_str(steps[s][2]), F1(duty_cur), F1(freq_cur));
  if (skipped) out("  (+%lu steps not shown)", (unsigned long)skipped);
  out("\r\n");
}

// Full table with measured pin levels for phase modes m3 (manual + phase test).
// Returns true if every gate pin matched.
static bool print_modes_table(const uint8_t m3[3], const char *title, const char *keys) {
  // Let the duty ramp finish so the measurement shows the final value.
  uint32_t t = ms_ticks;
  while (duty_cur != duty_target && (ms_ticks - t) < 1500) tud_task();

  uint32_t hi[6] = {0};
  const uint32_t n = 2048;                  // ~2 ms = ~40 PWM periods
  sample_pins(n, hi);

  float d = duty_applied;
  float avg = vin_volts * d / 100.0f;
  out("\r\n=== %s  duty %d.%d%%  VIN %d V ===\r\n", title, F1(d), (int)vin_volts);
  out(" phase  state         INH    INL     at J2 vs PGND (scope)         measured INH / INL\r\n");
  int bad = 0;
  for (int ph = 0; ph < 3; ph++) {
    uint8_t m = m3[ph];
    int ph_hi = (int)((hi[2 * ph] * 100 + n / 2) / n);
    int pl_hi = (int)((hi[2 * ph + 1] * 100 + n / 2) / n);
    if (!pct_ok(ph_hi, expected_pct_m(m3, 2 * ph, d)) ||
        !pct_ok(pl_hi, expected_pct_m(m3, 2 * ph + 1, d))) bad++;

    char scope[40];
    if (m == P)
      snprintf(scope, sizeof scope, "0-%dV pulses %d%%, avg %d.%dV",
               (int)vin_volts, (int)(d + 0.5f), F1(avg));
    else if (m == L)
      snprintf(scope, sizeof scope, "0 V (pulled low)");
    else
      snprintf(scope, sizeof scope, "floating (no motor = noise)");

    out("   %c    %-12s  %-5s  %-5s   %-28s  %3d%% / %3d%%\r\n",
        'A' + ph, phase_state_str(m),
        m == P ? "PWM" : "0", m == P ? "~PWM" : m == L ? "1" : "0",
        scope, ph_hi, pl_hi);
  }

  // Line-to-line expectation when one phase switches and one is held low.
  int hp = -1, lp = -1;
  for (int ph = 0; ph < 3; ph++) {
    if (m3[ph] == P) hp = ph;
    if (m3[ph] == L) lp = ph;
  }
  if (hp >= 0 && lp >= 0)
    out(" across %c-%c (DMM DC, red %c black %c): ~+%d.%dV avg  | scope: CH1 %c, CH2 %c, math CH1-CH2\r\n",
        'A' + hp, 'A' + lp, 'A' + hp, 'A' + lp, F1(avg), 'A' + hp, 'A' + lp);
  else if (hp >= 0)
    out(" %c vs PGND (DMM DC): ~%d.%dV avg (other phases off)\r\n", 'A' + hp, F1(avg));

  out(" gate pin check: %s   nFAULT: %s   %s\r\n", bad ? "MISMATCH" : "OK",
      fault_active() ? "LOW (FAULT)" : "high (ok)", keys);
  return bad == 0;
}

static void print_step_table(uint8_t s) {
  char title[48];
  snprintf(title, sizeof title, "step %d  [%s]  %s", s, step_names[s], dir_fwd ? "fwd" : "rev");
  print_modes_table(steps[s], title, "[n/+ next, b/- back, 0-5 jump, 'auto' resume]");
}

// ---------------------------------------------------------------- commands
static void reboot_to_bootloader(void) {
  motor_stop();
  out("\r\nRebooting into DFU bootloader...\r\n");
  delay_ms_usb(100);
  boot_request = BOOTLOADER_MAGIC;
  NVIC_SystemReset();
}

static bool watch_on, log_on, watch_reset;

static void print_status(void) {
  float f = freq_cur, ft = freq_target;
  float il = i_limit, ifl = i_filt, ip = i_peak, da = duty_applied, dc = duty_cap;
  out("current: I=%d.%02d A (peak %d.%02d A)  limit %d.%d A  duty applied %d.%d%% (limiter cap %d.%d%%)\r\n",
      F2(ifl), F2(ip), F1(il), F1(da), F1(dc));
  out("CSA %s, offsets A %d.%02d B %d.%02d C %d.%02d V\r\n",
      csa_ok ? "OK" : "NOT USABLE (duty capped at 8%)",
      F2(cs_off[0]), F2(cs_off[1]), F2(cs_off[2]));
  out("state=%s step=%d [%s] dir=%s nFAULT=%s steps=%lu watch=%s log=%s\r\n",
      state_names[state], step, step_names[step], dir_fwd ? "fwd" : "rev",
      fault_active() ? "LOW(fault)" : "high(ok)", (unsigned long)step_count,
      watch_on ? "on" : "off", log_on ? "on" : "off");
  out("duty=%d.%d%% (target %d.%d%%, max %d%%)  freq=%d.%d Hz (target %d.%d Hz)\r\n",
      F1(duty_cur), F1(duty_target), (int)DUTY_MAX_PCT, F1(f), F1(ft));
  out("=> %lu rpm (target %lu rpm) @ %lu pole pairs  accel=%d.%d Hz/s  align=%lu ms\r\n",
      (unsigned long)(f * 60.0f / pole_pairs), (unsigned long)(ft * 60.0f / pole_pairs),
      (unsigned long)pole_pairs, F1(accel), (unsigned long)align_ms);
}

static void print_help(void) {
  out("\r\n6-step open-loop BLDC driver (Ctrl-C or ESC = stop now)\r\n");
  out("  start / stop        auto-commutate (align, then ramp) / stop\r\n");
  out("  manual              hold the current step; then step by hand:\r\n");
  out("                        n or +  next step,  b or -  previous,  0..5  jump\r\n");
  out("  auto                leave manual mode and commutate again\r\n");
  out("  duty <0-%d>         PWM duty %% (start low with a motor, e.g. 5-10)\r\n", (int)DUTY_MAX_PCT);
  out("  freq <Hz>           target electrical frequency (0.5-%d)\r\n", (int)FREQ_MAX_HZ);
  out("  rpm <n> | pp <n>    target mechanical rpm | motor pole pairs\r\n");
  out("  accel <Hz/s>        frequency ramp rate\r\n");
  out("  align <ms>          rotor alignment time before spinning\r\n");
  out("  dir                 toggle direction\r\n");
  out("  watch               print every step in auto mode (try freq 1)\r\n");
  out("  log                 1 s summary + per-step pin check in auto mode\r\n");
  out("  vin <V>             supply voltage, for the expected-scope column\r\n");
  out("  ph ab | ph a | ph off | ph +/- | ph exit   phase/pair test (or run phase_test.py)\r\n");
  out("  ilim <A>            motor current limit (duty is trimmed to hold it)\r\n");
  out("  diag                DRV8316 fault diagnostics (nFAULT, nSLEEP, rails)\r\n");
  out("  status | help | dfu\r\n");
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

// ---------------------------------------------------------------- diagnostics
// ADC1: PA0/PA1/PA2 = SOA/SOB/SOC (IN1..3), VREFINT (IN18).
// ADC2: PA6 = nFAULT (IN3), read as a voltage to tell "driven low by the
// DRV's open-drain" (~0 V) from "clamped by an unpowered chip" (a few 100 mV).
#define VREFINT_CAL (*(const uint16_t *)0x1FFF75AAu)   // raw @ VDDA = 3.0 V

static void adc_enable(ADC_TypeDef *a) {
  a->CR &= ~ADC_CR_DEEPPWD;
  a->CR |= ADC_CR_ADVREGEN;
  busy_wait_us(30);
  a->CR |= ADC_CR_ADCAL;
  while (a->CR & ADC_CR_ADCAL) {}
  busy_wait_us(5);
  a->ISR = ADC_ISR_ADRDY;
  a->CR |= ADC_CR_ADEN;
  while (!(a->ISR & ADC_ISR_ADRDY)) {}
  a->SMPR1 = 0x3FFFFFFFu;                 // 640.5-cycle sampling on every channel
  a->SMPR2 = 0x07FFFFFFu;
}

static void adc_init_once(void) {
  static bool done;
  if (done) return;
  done = true;
  RCC->AHB2ENR |= RCC_AHB2ENR_ADC12EN;
  RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_ADC12SEL) | RCC_CCIPR_ADC12SEL_1;  // SYSCLK
  (void)RCC->AHB2ENR;
  ADC12_COMMON->CCR = ADC_CCR_VREFEN;
  busy_wait_us(20);
  adc_enable(ADC1);
  adc_enable(ADC2);
  // SOA/SOB/SOC (ADC1 IN1..3) are low-impedance: 47.5-cycle sampling (~4 us).
  ADC1->SMPR1 = (ADC1->SMPR1 & ~(0x1FFu << 3)) | (4u << 3) | (4u << 6) | (4u << 9);
}

static uint32_t adc_sum(ADC_TypeDef *a, uint32_t ch, int n) {
  uint32_t sum = 0;
  a->SQR1 = ch << ADC_SQR1_SQ1_Pos;       // one conversion
  for (int i = 0; i < n; i++) {
    a->ISR = ADC_ISR_EOC;
    a->CR |= ADC_CR_ADSTART;
    while (!(a->ISR & ADC_ISR_EOC)) {}
    sum += a->DR;
  }
  return sum;
}

static uint32_t adc_read16(ADC_TypeDef *a, uint32_t ch) { return adc_sum(a, ch, 16); }

static float vdda_volts(void) {
  return 3.0f * VREFINT_CAL * 16.0f / (float)adc_read16(ADC1, 18);
}

static float adc_volts(ADC_TypeDef *a, uint32_t ch, float vdda) {
  return vdda * (float)adc_read16(a, ch) / (16.0f * 4095.0f);
}

// Zero-current offsets of the three CSAs. Call only with the bridge off.
static void csa_calibrate(void) {
  adc_init_once();
  vdda_cal = vdda_volts();
  bool ok = true;
  for (int ph = 0; ph < 3; ph++) {
    cs_off[ph] = adc_volts(ADC1, (uint32_t)ph + 1, vdda_cal);
    if (cs_off[ph] < 1.3f || cs_off[ph] > 2.0f) ok = false;   // expect ~VREF/2
  }
  csa_ok = ok;
}

static float nfault_volts(float vdda) {
  gpio_mode(GPIOA, PIN_FAULT, 3);         // analog for the measurement
  float v = adc_volts(ADC2, 3, vdda);
  gpio_mode(GPIOA, PIN_FAULT, 0);
  return v;
}


static void report_analog(const char *label) {
  float vdda = vdda_volts();
  out("  [%s] 3V3 rail %d.%02d V | nFAULT %s, %d.%02d V | SOA %d.%02d SOB %d.%02d SOC %d.%02d V"
      " | nSLEEP %s DRVOFF %s\r\n",
      label, F2(vdda), fault_active() ? "LOW" : "high", F2(nfault_volts(vdda)),
      F2(adc_volts(ADC1, 1, vdda)), F2(adc_volts(ADC1, 2, vdda)), F2(adc_volts(ADC1, 3, vdda)),
      (GPIOC->ODR & (1u << PIN_SLEEP)) ? "high" : "LOW",
      (GPIOA->ODR & (1u << PIN_DRVOFF)) ? "high" : "LOW");
}

// Watches nFAULT for dur_ms (blocking, us resolution) and prints every edge.
static int watch_fault(uint32_t dur_ms, const char *label) {
  const uint32_t cpu = SystemCoreClock / 1000000u;
  struct { uint32_t us; uint8_t hi; } ev[12];
  int ne = 0, changes = 0;
  int start_hi = !fault_active(), hi = start_hi;
  uint32_t t0 = DWT->CYCCNT, dur = dur_ms * 1000u * cpu;
  while ((DWT->CYCCNT - t0) < dur) {
    int h = !fault_active();
    if (h != hi) {
      if (ne < 12) { ev[ne].us = (DWT->CYCCNT - t0) / cpu; ev[ne].hi = (uint8_t)h; ne++; }
      hi = h;
      changes++;
    }
  }
  char buf[200];
  int k = snprintf(buf, sizeof buf, "  %-34s nFAULT %s", label, start_hi ? "high" : "LOW");
  for (int i = 0; i < ne && k < (int)sizeof buf - 40; i++)
    k += snprintf(buf + k, sizeof buf - k, " -> %s @%lu.%lums", ev[i].hi ? "high" : "LOW",
                  (unsigned long)(ev[i].us / 1000), (unsigned long)(ev[i].us / 100 % 10));
  if (changes > ne) k += snprintf(buf + k, sizeof buf - k, " (+%d more edges)", changes - ne);
  snprintf(buf + k, sizeof buf - k, "  [end %s]\r\n", hi ? "high" : "LOW");
  out("%s", buf);
  return changes;
}

static void run_diag(void) {
  motor_stop();                           // DRVOFF high, all INx low
  adc_init_once();
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  out("\r\n=== DRV8316 diagnostics (bridge off: all INx low, DRVOFF low) ===\r\n");
  report_analog("start");
  float vdda = vdda_volts();
  float v_start = nfault_volts(vdda);
  bool low_start = fault_active();

  GPIOC->BRR = 1u << PIN_SLEEP;           // 1: fault-clear pulse
  busy_wait_us(30);
  GPIOC->BSRR = 1u << PIN_SLEEP;
  int c1 = watch_fault(20, "30 us nSLEEP reset pulse, 20 ms:");

  GPIOC->BRR = 1u << PIN_SLEEP;           // 2: short sleep
  delay_ms_usb(10);
  bool sleep_low = fault_active();
  report_analog("asleep 10 ms");
  GPIOC->BSRR = 1u << PIN_SLEEP;
  int c2 = watch_fault(300, "wake after 10 ms sleep, 300 ms:");

  GPIOC->BRR = 1u << PIN_SLEEP;           // 3: long sleep (full digital reset)
  delay_ms_usb(1000);
  bool sleep_low2 = fault_active();
  report_analog("asleep 1 s");
  GPIOC->BSRR = 1u << PIN_SLEEP;
  int c3 = watch_fault(300, "wake after 1 s sleep, 300 ms:");

  GPIOA->BSRR = 1u << PIN_DRVOFF;         // 4: DRVOFF high (datasheet: may pull nFAULT low)
  int c4 = watch_fault(50, "DRVOFF high, 50 ms:");
  GPIOA->BRR = 1u << PIN_DRVOFF;
  watch_fault(20, "DRVOFF back low, 20 ms:");

  delay_ms_usb(5);
  report_analog("end");

  bool low_end = fault_active();
  bool reacts = c1 || c2 || c3 || (sleep_low != low_start) || (sleep_low2 != low_start);
  (void)c4;
  out("\r\n  verdict: ");
  if (!low_end && !low_start)
    out("nFAULT is high and stays high: driver OK, nothing latched.\r\n");
  else if (!low_end)
    out("fault was latched and is now CLEARED by the nSLEEP pulse/sleep cycle. Driver OK.\r\n");
  else if (!reacts && v_start > 0.15f)
    out("nFAULT sits at %d.%02d V and ignores nSLEEP: the DRV8316 looks UNPOWERED\r\n"
        "  (pin clamped by its ESD diodes). VM is not reaching the chip: check TP2/TP3,\r\n"
        "  the PTC F1 and Q1, and J1.\r\n", F2(v_start));
  else if (!reacts)
    out("nFAULT is hard low (%d.%02d V) and ignores sleep/wake: either the DRV is\r\n"
        "  unpowered, or the nFAULT net (PA6 / R6 / DRV pin 22) is shorted to GND.\r\n", F2(v_start));
  else
    out("DRV responds to nSLEEP (powered) but the fault comes back after wake with\r\n"
        "  the bridge off: a SUPPLY-side fault (VM undervoltage, charge pump UV,\r\n"
        "  AVDD or internal buck UV). Check VM at TP3, C26/C27 (charge pump), C24\r\n"
        "  (AVDD ~3.3 V) and R5/C29 (internal buck ~3.3 V).\r\n");
#if AUTOSTART
  const char *err = motor_start();
  out("  restarting commutation: %s\r\n", err ? err : "ok");
#endif
}

static void handle_line(char *line) {
  char *cmd = strtok(line, " \t");
  char *arg = strtok(NULL, " \t");
  if (!cmd) return;
  float v = arg ? strtof(arg, NULL) : 0.0f;

  if (!strcmp(cmd, "start")) {
    const char *err = motor_start();
    if (err) out("start refused: %s\r\n", err);
    else out("aligning for %lu ms at %d.%d%% duty...\r\n", (unsigned long)align_ms, F1(duty_target));
  } else if (!strcmp(cmd, "stop")) {
    motor_stop();
    out("stopped\r\n");
  } else if (!strcmp(cmd, "manual")) {
    const char *err = manual_enter();
    if (err) out("manual refused: %s\r\n", err);
    else print_step_table(step);
  } else if (!strcmp(cmd, "auto")) {
    if (state != M_MANUAL) out("not in manual mode (use start)\r\n");
    else {
      __disable_irq();
      comm_timer_start();
      __enable_irq();
      out("auto commutation from %d.%d Hz -> %d.%d Hz\r\n", F1(freq_cur), F1(freq_target));
    }
  } else if (!strcmp(cmd, "duty") && arg) {
    duty_target = clampf(v, 0, DUTY_MAX_PCT);
    out("duty target %d.%d%%\r\n", F1(duty_target));
    if (state == M_MANUAL) print_step_table(step);
  } else if (!strcmp(cmd, "freq") && arg) {
    freq_target = clampf(v, FREQ_MIN_HZ, FREQ_MAX_HZ);
    out("freq target %d.%d Hz (%d.%d steps/s)\r\n", F1(freq_target), F1(6 * freq_target));
  } else if (!strcmp(cmd, "rpm") && arg) {
    freq_target = clampf(v * pole_pairs / 60.0f, FREQ_MIN_HZ, FREQ_MAX_HZ);
    out("freq target %d.%d Hz\r\n", F1(freq_target));
  } else if (!strcmp(cmd, "pp") && arg && v >= 1) {
    pole_pairs = (uint32_t)v;
    out("pole pairs %lu\r\n", (unsigned long)pole_pairs);
  } else if (!strcmp(cmd, "accel") && arg) {
    accel = clampf(v, 0.1f, 5000.0f);
    out("accel %d.%d Hz/s\r\n", F1(accel));
  } else if (!strcmp(cmd, "align") && arg) {
    align_ms = (uint32_t)clampf(v, 0, 5000);
    out("align %lu ms\r\n", (unsigned long)align_ms);
  } else if (!strcmp(cmd, "dir")) {
    if (state == M_RUN || state == M_ALIGN) out("stop the motor first\r\n");
    else { dir_fwd = !dir_fwd; out("direction %s\r\n", dir_fwd ? "fwd" : "rev"); }
  } else if (!strcmp(cmd, "watch")) {
    watch_on = !watch_on;
    watch_reset = true;
    out("watch %s\r\n", watch_on ? "on" : "off");
  } else if (!strcmp(cmd, "log")) {
    log_on = !log_on;
    out("log %s\r\n", log_on ? "on" : "off");
  } else if (!strcmp(cmd, "vin") && arg) {
    vin_volts = clampf(v, 0, 40);
    out("vin %d V (display only)\r\n", (int)vin_volts);
  } else if (!strcmp(cmd, "status")) {
    print_status();
  } else if (!strcmp(cmd, "help") || !strcmp(cmd, "?")) {
    print_help();
  } else if (!strcmp(cmd, "dfu")) {
    reboot_to_bootloader();
  } else if (!strcmp(cmd, "ph")) {
    // ph a | ph ab (A PWM, B low) | ph off | ph + | ph - | ph exit
    if (arg && !strcmp(arg, "exit")) {
      motor_stop();
#if AUTOSTART
      const char *err = motor_start();
      out("phase test done, back to 6-step: %s\r\n", err ? err : "ok");
#else
      out("phase test done, stopped\r\n");
#endif
      return;
    }
    const char *err = phase_enter();
    if (err) { out("phase test refused: %s\r\n", err); return; }
    if (!arg || !strcmp(arg, "off")) {
      memset(pt_modes, Z, sizeof pt_modes);
    } else if (!strcmp(arg, "+") || !strcmp(arg, "-")) {
      duty_target = clampf(duty_target + (arg[0] == '+' ? 5.0f : -5.0f), 0, DUTY_MAX_PCT);
    } else {
      size_t len = strlen(arg);
      int hp = (len >= 1) ? arg[0] - 'a' : -1;
      int lp = (len == 2) ? arg[1] - 'a' : -1;
      if (len > 2 || hp < 0 || hp > 2 || (len == 2 && (lp < 0 || lp > 2 || lp == hp))) {
        out("usage: ph a|b|c|ab|ba|bc|cb|ac|ca|off|+|-|exit\r\n");
        return;
      }
      memset(pt_modes, Z, sizeof pt_modes);
      pt_modes[hp] = P;
      if (lp >= 0) pt_modes[lp] = L;
    }
    __disable_irq();
    load_modes(pt_modes);
    __enable_irq();
    char title[48];
    int hp = -1, lp = -1;
    for (int ph = 0; ph < 3; ph++) { if (pt_modes[ph] == P) hp = ph; if (pt_modes[ph] == L) lp = ph; }
    if (hp < 0) snprintf(title, sizeof title, "PHASE TEST: all off");
    else if (lp < 0) snprintf(title, sizeof title, "PHASE TEST: %c alone", 'A' + hp);
    else snprintf(title, sizeof title, "PHASE TEST: %c-%c  (%c+ %c-)", 'A' + hp, 'A' + lp, 'A' + hp, 'A' + lp);
    print_modes_table(pt_modes, title, "");
  } else if (!strcmp(cmd, "ilim") && arg) {
    i_limit = clampf(v, 0.2f, 3.0f);
    out("current limit %d.%d A (trip at %d.%d A)\r\n", F1(i_limit),
        F1(i_limit < 2.0f ? 2 * i_limit : 4.0f));
  } else if (!strcmp(cmd, "diag")) {
    run_diag();
  } else {
    out("? unknown command (type help)\r\n");
  }
}

static char line[48];
static uint8_t line_len;

static void handle_rx(void) {
  uint8_t rx[64];
  uint32_t n = tud_cdc_read(rx, sizeof rx);

  for (uint32_t i = 0; i < n; i++) {
    char c = (char)rx[i];
    if (c == 0x03 || c == 0x1B) {         // Ctrl-C / ESC: emergency stop
      motor_stop();
      line_len = 0;
      out("\r\nSTOP\r\n");
    } else if (state == M_MANUAL && line_len == 0 &&
               (c == 'n' || c == '+' || c == 'b' || c == '-' || (c >= '0' && c <= '5'))) {
      uint8_t s = step;
      if (c == 'n' || c == '+')      s = (uint8_t)((s + 1) % 6);
      else if (c == 'b' || c == '-') s = (uint8_t)((s + 5) % 6);
      else                           s = (uint8_t)(c - '0');
      manual_goto(s);
      print_step_table(s);
    } else if (c == '\r' || c == '\n') {
      out("\r\n");
      line[line_len] = '\0';
      line_len = 0;
      handle_line(line);
    } else if ((c == 0x08 || c == 0x7F) && line_len) {
      line_len--;
      out("\b \b");
    } else if (c >= 0x20 && line_len < sizeof line - 1) {
      line[line_len++] = c;
      tud_cdc_write_char(c);              // echo
      tud_cdc_write_flush();
    }
  }
}

// ---------------------------------------------------------------- main
int main(void) {
  jump_to_bootloader_if_requested();

  clock_init();
  driver_io_init();
  pwm_init();
  motor_stop();
  tud_init(0);

#if AUTOSTART
  duty_target = AUTO_DUTY_PCT;
  freq_target = AUTO_FREQ_HZ;
#endif
  uint32_t fault_at = 0, faults = 0, retry_delay = FAULT_RETRY_MS;
  bool retry_quiet = false;
#if AUTOSTART
  delay_ms_usb(5);                          // > tWAKE (1 ms) after nSLEEP high
  if (motor_start()) fault_at = 1;          // driver not ready: keep retrying
#endif
  uint32_t last_log = 0, last_steps = 0;
  uint32_t last_watch_ms = 0, last_watch_count = 0;

  bool was_connected = false;
  while (1) {
    tud_task();
    pin_monitor_sample();

    bool conn = tud_cdc_connected();
    if (conn && !was_connected) {
      delay_ms_usb(50);
      print_help();
      print_status();
    }
    was_connected = conn;

    uint32_t ev;
    __disable_irq();
    ev = events;
    events = 0;
    __enable_irq();
    if (ev & EV_OVERCURRENT) {
      faults++;
      float il = i_limit;
      fault_at = AUTOSTART ? (ms_ticks ? ms_ticks : 1) : 0;
      retry_delay = OC_RETRY_MS;
      out("\r\n!! OVERCURRENT: above %d.%d A for 20 ms in %s (#%lu): stopped%s\r\n",
          F1(il < 2.0f ? 2 * il : 4.0f), state_names[fault_state], (unsigned long)faults,
          AUTOSTART ? ", retrying in 3 s" : "");
    }
    if (ev & EV_FAULT) {
      faults++;
      bool retry = AUTOSTART;
      fault_at = retry ? (ms_ticks ? ms_ticks : 1) : 0;
      retry_delay = FAULT_RETRY_MS;
      out("\r\n!! nFAULT asserted in %s (fault #%lu): stopped%s\r\n",
          state_names[fault_state], (unsigned long)faults,
          retry ? ", retrying in 1 s" : "; type start or manual to retry");
    }
    if (ev & EV_RUNNING) out("aligned, commutating from %d.%d Hz\r\n", F1(freq_cur));
    if (ev & EV_AT_SPEED) out("at target %d.%d Hz\r\n", F1(freq_target));

    if (fault_at && state == M_IDLE && (ms_ticks - fault_at) >= retry_delay) {
      const char *err = (fault_state == M_PHASE) ? phase_enter() : motor_start();
      if (!err || !retry_quiet)
        out("auto-restart: %s%s\r\n", err ? err : "ok", err ? " (retrying every 1 s, quietly)" : "");
      retry_quiet = err != NULL;
      fault_at = err ? (ms_ticks ? ms_ticks : 1) : 0;
    }

    if (log_on && state == M_RUN && (ms_ticks - last_log) >= LOG_PERIOD_MS) {
      last_log = ms_ticks;
      uint32_t sc = step_count;
      if (sc < last_steps) last_steps = 0;          // counter reset by start
      float ifl = i_filt, ip = i_peak, da = duty_applied;
      i_peak = 0;
      out("[%5lu.%lus] %s  steps/s=%lu (expect %d)  duty=%d.%d%%  freq=%d.%d Hz  "
          "I=%d.%02dA (peak %d.%02dA)  nFAULT=%s\r\n",
          (unsigned long)(ms_ticks / 1000), (unsigned long)(ms_ticks / 100 % 10),
          state_names[state], (unsigned long)(sc - last_steps), (int)(6 * freq_cur + 0.5f),
          F1(da), F1(freq_cur), F2(ifl), F2(ip),
          fault_active() ? "LOW" : "high");
      last_steps = sc;
      pin_monitor_report();
    }

    if (watch_reset) { watch_reset = false; last_watch_count = step_count; }
    if (watch_on && state == M_RUN && step_count != last_watch_count &&
        (ms_ticks - last_watch_ms) >= WATCH_MIN_GAP_MS) {
      uint32_t sc = step_count;
      print_step_compact(step, sc > last_watch_count + 1 ? sc - last_watch_count - 1 : 0);
      last_watch_count = sc;
      last_watch_ms = ms_ticks;
    }

    if (tud_cdc_available()) handle_rx();
  }
}
