/**
 * @file    main.c
 * @brief   FOC application with ESP32 telemetry/command link (foc_esp).
 *
 * This application extends the standalone "foc" app with a half-duplex
 * USART1 link to an external ESP32. The ESP32 serves a web interface that
 * lets a browser command the motor and observe real-time telemetry.
 *
 * Hardware wiring:
 *   - J7.1 (PB6) is used as the ESP32 half-duplex link.
 *   - J7.2 (PB7) stays as AS5600 I2C SDA (unchanged).
 *   - J6.2 (PA15) stays as AS5600 I2C SCL (unchanged).
 *
 * Wire the ESP32:
 *   ESP32 GPIO17 (TX) -> 1k resistor -> PB6 (J7.1)
 *   ESP32 GPIO16 (RX) -> PB6 (J7.1)      (same node as TX)
 *   Add a 4.7k pull-up from PB6 to 3.3V.
 *   ESP32 GND -> J6.3 (board GND).
 *   Power the ESP32 from USB (separate supply, common GND).
 *
 * Safety:
 *   The bridge stays off until the ESP32 sends a CMD with the ENABLE flag.
 *   If no valid CMD is received for LINK_WATCHDOG_MS, iq drops to zero;
 *   after LINK_DISABLE_MS the bridge is disabled.
 *
 * Flash with:
 *   make APP=foc_esp flash
 */
#include "board.h"
#include "debug.h"
#include "as5600.h"
#include "encoder.h"
#include "foc.h"
#include "esp_link.h"

#include <math.h>
#include <string.h>

/* ---- tune here ---------------------------------------------------------- */
#define APP_IQ_MAX_A        4.0f    /* hard limit: CSA saturates at ~4.6 A   */
#define APP_IQ_RAMP         2.0f    /* A/s ramp toward commanded value        */
#define APP_RPM_LIMIT       800.0f  /* encoder usable speed ceiling           */
#define APP_CAL_MOD         0.01f   /* open-loop modulation for calibration   */
#define APP_CAL_HOLD_MS     400u
#define APP_OFFSET_SAMPLES  500u
#define APP_ENC_ERR_WINDOW  20u     /* errors per 200 ms before stopping      */
#define LINK_WATCHDOG_MS    500u    /* zero iq if no CMD for this long         */
#define LINK_DISABLE_MS     1000u   /* disable bridge if link dead this long   */

#define TWO_PI_F            6.28318530718f

static volatile uint16_t g_dma[ADC_BUF_LEN];
static volatile uint16_t g_adc[ADC_BUF_LEN];
static volatile uint32_t g_isr_count;

/* ---- application state -------------------------------------------------- */
static volatile float    s_iq_actual = 0.0f;   /* latest measured iq (A) */
static volatile float    s_id_actual = 0.0f;
static volatile float    s_rpm       = 0.0f;
static volatile uint16_t s_angle_raw = 0u;

static uint8_t  s_bridge_enabled = 0u;
static float    s_iq_target  = 0.0f;   /* latest commanded (A) */
static float    s_id_target  = 0.0f;
static float    s_iq_cmd     = 0.0f;   /* ramped value */
static uint16_t s_rpm_limit  = 0u;     /* 0 = use APP_RPM_LIMIT */

/* -------------------------------------------------------------------------- */
/* HAL callbacks                                                              */
/* -------------------------------------------------------------------------- */
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1)
    {
        for (uint32_t i = 0u; i < ADC_BUF_LEN; i++)
        {
            g_adc[i] = g_dma[i];
        }
        g_isr_count++;
        FOC_ControlISR(g_adc);
        s_iq_actual  = FOC_GetIq();
        s_id_actual  = FOC_GetId();
        s_rpm        = Encoder_MechanicalSpeed() * (60.0f / TWO_PI_F);
        s_angle_raw  = AS5600_GetRaw();
    }
}

void HAL_I2C_MemRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        AS5600_RxComplete();
        Encoder_OnSample(AS5600_GetRaw());
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c)
{
    if (hi2c->Instance == I2C1)
    {
        AS5600_Error();
    }
}

/* -------------------------------------------------------------------------- */
/* helpers                                                                    */
/* -------------------------------------------------------------------------- */
static void start_adc(void)
{
    if (HAL_TIM_Base_Start(&htim1) != HAL_OK)
    {
        Error_Handler();
    }
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_dma, ADC_BUF_LEN) != HAL_OK)
    {
        Error_Handler();
    }
}

static void stop_motor(const char *why)
{
    FOC_Disable();
    Board_DrvOutputDisable();
    s_bridge_enabled = 0u;
    DBG_Printf("STOP: %s\r\n", why);
    DBG_Puts("Bridge disabled. Reset to run.\r\n");
    for (;;)
    {
        Board_DelayMs(1000u);
    }
}

static void banner(void)
{
    DBG_Puts("\r\n\r\n");
    DBG_Puts("==================================================\r\n");
    DBG_Puts(" bldc_control_final_v6  |  foc_esp\r\n");
    DBG_Puts(" STM32G431CBT6 + DRV8316CT + AS5600 + ESP32 link\r\n");
    DBG_Puts("==================================================\r\n");
    DBG_Printf("build : %s %s\r\n", __DATE__, __TIME__);
    DBG_Printf("SYSCLK: %lu Hz\r\n", (unsigned long)HAL_RCC_GetSysClockFreq());
    DBG_Puts("ESP32 link on PB6 (J7.1), 230400 baud, half-duplex.\r\n");
    DBG_Puts("Bridge DISABLED until ESP32 sends ENABLE command.\r\n");
}

static void enable_bridge(void)
{
    if (s_bridge_enabled)
    {
        return;
    }
    FOC_SetCurrentRef(0.0f, 0.0f);
    FOC_Enable();
    if (!FOC_IsEnabled())
    {
        stop_motor("FOC_Enable refused");
    }
    Board_DrvOutputEnable();
    s_bridge_enabled = 1u;
    DBG_Puts("[link] bridge ENABLED\r\n");
}

static void disable_bridge(void)
{
    if (!s_bridge_enabled)
    {
        return;
    }
    FOC_SetCurrentRef(0.0f, 0.0f);
    FOC_Disable();
    Board_DrvOutputDisable();
    s_bridge_enabled = 0u;
    s_iq_cmd = 0.0f;
    DBG_Puts("[link] bridge DISABLED\r\n");
}

/* -------------------------------------------------------------------------- */
int main(void)
{
    uint8_t  enc_status = 0u;
    uint32_t t_prev;
    uint32_t t_print;
    uint32_t err_prev;

    Board_Init();

    /* Encoder */
    Board_InitI2C1();
    if (AS5600_Init(&hi2c1, &enc_status) != 0)
    {
        DBG_Puts("\r\n\r\nAS5600 not found. Check J6/J7 wiring and 4.7k pull-ups.\r\n");
        Error_Handler();
    }
    Encoder_Init();

    /* Driver */
    Board_DrvWake();
    Board_DelayMs(5u);
    if (DRV8316_Init() != 0)
    {
        DBG_Puts("\r\nDRV8316 fault after wake. Check VM and GND.\r\n");
        Error_Handler();
    }

    /* TIM1 + ADC */
    Board_InitTim1(FOC_PWM_ARR);
    Board_Tim1AdcTrigger();
    Board_InitAdc1();

    /* FOC */
    FOC_Init(&htim1);

    /* Banner (uses standard debug UART TX before link init) */
    banner();

    /* Encoder calibration */
    if ((enc_status & AS5600_ST_MD) == 0u)
    {
        DBG_Puts("WARNING: AS5600 no magnet detected.\r\n");
    }

    if (Encoder_GetCal()->valid == 0u)
    {
        int rc;
        DBG_Puts("No encoder calibration. Calibrating (rotor must be free)...\r\n");
        Board_DrvOutputEnable();
        rc = Encoder_Calibrate(FOC_ApplyOpenLoopVector, APP_CAL_HOLD_MS, APP_CAL_MOD);
        Board_DrvOutputDisable();
        Board_Tim1StopPwm();
        if (rc != 0)
        {
            DBG_Printf("Encoder_Calibrate rc=%d (see foc/README.md)\r\n", rc);
            Error_Handler();
        }
        DBG_Printf("Cal OK: sign=%+.0f offset=%.4f rad\r\n",
                   (double)Encoder_GetCal()->sign,
                   (double)Encoder_GetCal()->offset);
    }
    else
    {
        DBG_Printf("Cal loaded: sign=%+.0f offset=%.4f rad\r\n",
                   (double)Encoder_GetCal()->sign,
                   (double)Encoder_GetCal()->offset);
    }

    /* Current sense offset calibration */
    start_adc();
    Board_DelayMs(20u);
    DBG_Puts("Measuring current offsets (bridge off)...\r\n");
    FOC_CalibrateOffsets(g_adc, APP_OFFSET_SAMPLES);
    if (!FOC_IsReady())
    {
        stop_motor("offset calibration failed");
    }

    /* Prime encoder angle tracker */
    for (uint32_t i = 0u; i < 8u; i++)
    {
        uint16_t raw = 0u;
        if (AS5600_ReadBlocking(&raw, 20u) == 0)
        {
            Encoder_OnSample(raw);
        }
        Board_DelayMs(2u);
    }
    if (!Encoder_GetState()->valid)
    {
        stop_motor("encoder produced no valid samples");
    }

    DBG_Puts("Switching USART1 to 230400 baud half-duplex link...\r\n");
    DBG_Puts("Waiting for ENABLE command from web UI...\r\n");

    /* Reconfigure USART1 to half-duplex link mode (no further debug prints after this) */
    EspLink_Init(LINK_UART_BAUD);

    t_prev   = Board_Millis();
    t_print  = t_prev;
    err_prev = AS5600_GetErrorCount();

    for (;;)
    {
        uint32_t now = Board_Millis();
        float    dt  = (float)(now - t_prev) * 0.001f;
        EspCmd   cmd;
        uint8_t  seq;
        EspTlm   tlm;
        uint8_t  tlm_flags = 0u;
        float    rpm_lim;

        if (dt <= 0.0f)
        {
            dt = 0.001f;
        }
        t_prev = now;

        /* ---- DRV fault monitoring --------------------------------------- */
        if (Board_DrvFault() != 0u)
        {
            (void)DRV8316_ClearFault();
            if (Board_DrvFault() != 0u)
            {
                stop_motor("DRV8316 nFAULT persistent");
            }
        }

        /* ---- link watchdog ---------------------------------------------- */
        if (s_bridge_enabled && !EspLink_IsAlive(LINK_WATCHDOG_MS))
        {
            s_iq_target = 0.0f;
            s_id_target = 0.0f;
        }
        if (s_bridge_enabled && !EspLink_IsAlive(LINK_DISABLE_MS))
        {
            disable_bridge();
        }

        /* ---- process incoming command ------------------------------------ */
        if (EspLink_TakeCmd(&cmd, &seq))
        {
            float iq_req = (float)cmd.iq_ma * 0.001f;
            float id_req = (float)cmd.id_ma * 0.001f;

            if (iq_req >  APP_IQ_MAX_A) iq_req =  APP_IQ_MAX_A;
            if (iq_req < -APP_IQ_MAX_A) iq_req = -APP_IQ_MAX_A;

            if (cmd.flags & ESPL_CMD_FLAG_CLEAR_FAULT)
            {
                (void)DRV8316_ClearFault();
            }

            if (cmd.flags & ESPL_CMD_FLAG_ENABLE)
            {
                enable_bridge();
                s_iq_target = iq_req;
                s_id_target = id_req;
                s_rpm_limit = cmd.rpm_limit;
            }
            else
            {
                s_iq_target = 0.0f;
                s_id_target = 0.0f;
                disable_bridge();
            }

            /* Reply with telemetry for this poll cycle */
            rpm_lim = (s_rpm_limit > 0u) ? (float)s_rpm_limit : APP_RPM_LIMIT;

            if (s_bridge_enabled)        tlm_flags |= ESPL_TLM_FLAG_ENABLED;
            if (Board_DrvFault())        tlm_flags |= ESPL_TLM_FLAG_FAULT;
            if (Encoder_GetCal()->valid) tlm_flags |= ESPL_TLM_FLAG_CAL_VALID;
            if (EspLink_IsAlive(LINK_WATCHDOG_MS)) tlm_flags |= ESPL_TLM_FLAG_LINK_OK;
            if (fabsf(s_rpm) >= rpm_lim) tlm_flags |= ESPL_TLM_FLAG_RPM_LIMITED;
            if (AS5600_GetErrorCount() > 0u) tlm_flags |= ESPL_TLM_FLAG_ENC_ERROR;

            tlm.flags      = tlm_flags;
            tlm.drv_fault  = Board_DrvFault();
            tlm.id_ma      = (int16_t)(s_id_actual * 1000.0f);
            tlm.iq_ma      = (int16_t)(s_iq_actual * 1000.0f);
            tlm.iq_ref_ma  = (int16_t)(s_iq_cmd * 1000.0f);
            tlm.rpm_x10    = (int16_t)(s_rpm * 10.0f);
            tlm.angle_raw  = s_angle_raw;
            tlm.uptime_ms  = now;
            tlm.enc_errors = (uint16_t)(AS5600_GetErrorCount() & 0xFFFFu);
            tlm.isr_loops  = (uint16_t)(g_isr_count / 1000u);

            EspLink_SendTlm(&tlm, seq);
        }

        /* ---- RPM guard + iq ramp ---------------------------------------- */
        if (s_bridge_enabled)
        {
            rpm_lim = (s_rpm_limit > 0u) ? (float)s_rpm_limit : APP_RPM_LIMIT;

            if (fabsf(s_rpm) >= rpm_lim)
            {
                s_iq_target = 0.0f;
            }

            float step = APP_IQ_RAMP * dt;
            if (s_iq_cmd < s_iq_target - step)       s_iq_cmd += step;
            else if (s_iq_cmd > s_iq_target + step)   s_iq_cmd -= step;
            else                                       s_iq_cmd  = s_iq_target;

            FOC_SetCurrentRef(s_id_target, s_iq_cmd);
        }

        /* ---- encoder error monitoring ----------------------------------- */
        if ((now - t_print) >= 2000u)
        {
            uint32_t err  = AS5600_GetErrorCount();
            uint32_t derr = err - err_prev;

            t_print  = now;
            err_prev = err;

            if (derr > (uint32_t)APP_ENC_ERR_WINDOW * 10u)
            {
                stop_motor("encoder I2C error rate high");
            }
        }

        Board_DelayMs(1u);
    }
}
