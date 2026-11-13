/**
 * @file    main.c
 * @brief   Final sensored FOC application for bldc_control_final_v6.
 *
 * Chain:
 *   AS5600 (I2C1) -> encoder.c -> electrical angle
 *   ADC1 PA0/PA1/PA2 (SOA/SOB/SOC) -> foc.c -> TIM1 -> DRV8316R
 *
 * First run: if no valid encoder calibration is stored, the app performs
 * the sign/offset calibration itself (rotor must be free) and writes it to
 * the last flash page. Every later boot reads that record and starts
 * directly.
 *
 * This is a current-mode FOC with a fixed iq reference and a coarse upper
 * speed guard. It is a bring-up app, not a servo drive: there is no speed
 * or position loop. Stop conditions: DRV nFAULT, encoder I2C errors.
 *
 * Flash this with:
 *   make APP=foc flash
 */
#include "board.h"
#include "debug.h"
#include "as5600.h"
#include "encoder.h"
#include "drv8316.h"
#include "foc.h"

#include <math.h>

/* ---- tune here ---------------------------------------------------------- */
#define APP_IQ_A            1.0f    /* target q-axis current, A (0.071 Nm/A) */
#define APP_IQ_RAMP         0.5f    /* A/s, keeps the start gentle           */
#define APP_RPM_LIMIT       800.0f  /* AS5600 is good to ~1000 rpm only      */
#define APP_CAL_MOD         0.01f   /* ~2.2 A at standstill, do not raise    */
#define APP_CAL_HOLD_MS     400u
#define APP_OFFSET_SAMPLES  500u    /* 1 ms apart -> 0.5 s                  */
#define APP_ENC_ERR_PER_WINDOW 20u  /* I2C errors per 200 ms before stopping */

#define TWO_PI_F            6.28318530718f

static volatile uint16_t g_dma[ADC_BUF_LEN];
static volatile uint16_t g_adc[ADC_BUF_LEN];
static volatile uint32_t g_isr_count;

/* -------------------------------------------------------------------------- */
/* callbacks                                                                  */
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
        DBG_Puts("TIM1 base start failed.\r\n");
        Error_Handler();
    }
    if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)g_dma, ADC_BUF_LEN) != HAL_OK)
    {
        DBG_Puts("ADC DMA start failed.\r\n");
        Error_Handler();
    }
}

static void stop_motor(const char *why)
{
    FOC_Disable();
    Board_DrvOutputDisable();
    DBG_Printf("STOP: %s\r\n", why);
    DBG_Puts("Bridge disabled. Reset to run again.\r\n");
    for (;;)
    {
        Board_DelayMs(1000u);
    }
}

static void banner(void)
{
    DBG_Puts("\r\n\r\n");
    DBG_Puts("==================================================\r\n");
    DBG_Puts(" bldc_control_final_v6  |  foc\r\n");
    DBG_Puts(" STM32G431CBT6 + DRV8316R + AS5600\r\n");
    DBG_Puts("==================================================\r\n");
    DBG_Printf("build : %s %s\r\n", __DATE__, __TIME__);
    DBG_Printf("SYSCLK: %lu Hz\r\n", (unsigned long)HAL_RCC_GetSysClockFreq());
}

/* -------------------------------------------------------------------------- */
int main(void)
{
    uint8_t  status = 0u;
    uint32_t t_print;
    uint32_t t_prev;
    uint32_t err_prev;
    float    iq_cmd = 0.0f;
    float    rpm = 0.0f;
    float    id = 0.0f;
    float    iq = 0.0f;

    Board_Init();

    /* Encoder first: a missing magnet or a floating SDA must be obvious. */
    Board_InitI2C1();
    if (AS5600_Init(&hi2c1, &status) != 0)
    {
        DBG_Puts("\r\n\r\nAS5600 did not answer at 0x36.\r\n");
        DBG_Puts("Check J6.2=SCL, J7.2=SDA, J6.1=3V3, J6.3=GND and the\r\n");
        DBG_Puts("4.7k pull-ups. Flashing test_01_as5600 gives more detail.\r\n");
        Error_Handler();
    }
    Encoder_Init();

    /* Gate driver. It powers up in 6x mode with the slow 25 V/us slew, so
     * this must run before any PWM is applied. */
    Board_InitSPI1();
    Board_DrvWake();
    Board_DelayMs(5u);
    if (DRV8316_Init(&hspi1, DRV_NSCS_PORT, DRV_NSCS_PIN) != 0)
    {
        DBG_Puts("\r\nDRV8316 SPI configuration failed.\r\n");
        DBG_Puts("Run test_02_drv8316_spi before using this app.\r\n");
        Error_Handler();
    }

    /* 20 kHz centre-aligned PWM. The counter is started later, once the
     * offsets have been measured with the bridge off. */
    Board_InitTim1(PWM_ARR_20KHZ);
    Board_Tim1AdcTrigger();
    FOC_Init(&htim1);
    Board_InitAdc1();

    banner();
    DBG_Printf("AS5600: STATUS=0x%02X  magnet=%u\r\n", status,
               (unsigned)((status & AS5600_ST_MD) ? 1u : 0u));
    DBG_Printf("Encoder: cal valid=%u sign=%+.0f offset=%.4f rad\r\n",
               (unsigned)Encoder_GetCal()->valid,
               (double)Encoder_GetCal()->sign,
               (double)Encoder_GetCal()->offset);

    if ((status & AS5600_ST_MD) == 0u)
    {
        DBG_Puts("WARNING: no magnet detected. Motion will not be correct.\r\n");
    }

    /* ---- one-time encoder calibration ---------------------------------- */
    if (Encoder_GetCal()->valid == 0u)
    {
        int rc;

        DBG_Puts("\rNo valid encoder calibration. Calibrating now.\r\n");
        DBG_Puts("The rotor must be free to turn a few degrees.\r\n");

        Board_DrvOutputEnable();
        rc = Encoder_Calibrate(FOC_ApplyOpenLoopVector, APP_CAL_HOLD_MS,
                               APP_CAL_MOD);
        Board_DrvOutputDisable();
        Board_Tim1StopPwm();

        if (rc != 0)
        {
            DBG_Printf("Encoder_Calibrate rc=%d\r\n", rc);
            DBG_Puts("  -2: AS5600 read failed\r\n");
            DBG_Puts("  -3: rotor did not follow (phase order / open phase)\r\n");
            Error_Handler();
        }

        DBG_Printf("Calibrated: sign=%+.0f offset=%.4f rad\r\n",
                   (double)Encoder_GetCal()->sign,
                   (double)Encoder_GetCal()->offset);
    }

    /* ---- offsets, then run --------------------------------------------- */
    start_adc();
    Board_DelayMs(20u);                 /* let the DMA fill a few frames */

    DBG_Puts("Measuring zero-current offsets (bridge off)...\r\n");
    FOC_CalibrateOffsets(g_adc, APP_OFFSET_SAMPLES);

    if (FOC_IsReady() == 0u)
    {
        stop_motor("offset calibration did not run");
    }

    /* The control ISR only calls Encoder_Tick() once FOC is enabled, so prime
     * the angle tracker here. Without this the first cycles would run at
     * theta = 0. */
    for (uint32_t i = 0u; i < 8u; i++)
    {
        uint16_t raw = 0u;

        if (AS5600_ReadBlocking(&raw, 20u) == 0)
        {
            Encoder_OnSample(raw);
        }
        Board_DelayMs(2u);
    }
    if (Encoder_GetState()->valid == 0u)
    {
        stop_motor("encoder produced no valid samples");
    }

    FOC_SetCurrentRef(0.0f, 0.0f);
    FOC_Enable();
    if (FOC_IsEnabled() == 0u)
    {
        stop_motor("FOC_Enable refused (encoder calibration invalid)");
    }

    Board_DrvOutputEnable();

    DBG_Printf("Running: iq ramp to %.2f A, guard %.0f rpm.\r\n",
               (double)APP_IQ_A, (double)APP_RPM_LIMIT);
    DBG_Puts("\r\n   t_ms    rpm     id_A    iq_A   iq_ref  enc_err  nFAULT\r\n");

    t_prev  = Board_Millis();
    t_print = t_prev;
    err_prev = AS5600_GetErrorCount();

    for (;;)
    {
        uint32_t now = Board_Millis();
        float    dt  = (float)(now - t_prev) * 0.001f;

        if (dt <= 0.0f)
        {
            dt = 0.001f;
        }
        t_prev = now;

        if (Board_DrvFault() != 0u)
        {
            (void)DRV8316_ClearFault();
            if (Board_DrvFault() != 0u)
            {
                stop_motor("DRV8316 nFAULT");
            }
        }

        rpm = Encoder_MechanicalSpeed() * (60.0f / TWO_PI_F);

        if (fabsf(rpm) < APP_RPM_LIMIT)
        {
            iq_cmd += APP_IQ_RAMP * dt;
            if (iq_cmd > APP_IQ_A)
            {
                iq_cmd = APP_IQ_A;
            }
        }
        else
        {
            /* Above the encoder's usable band, just drop the torque and let
             * the motor coast down. Crude, but safe for a bring-up app. */
            iq_cmd = 0.0f;
        }

        FOC_SetCurrentRef(0.0f, iq_cmd);

        if ((now - t_print) >= 200u)
        {
            uint32_t err  = AS5600_GetErrorCount();
            uint32_t derr = err - err_prev;

            t_print  = now;
            err_prev = err;

            if (derr > APP_ENC_ERR_PER_WINDOW)
            {
                stop_motor("encoder I2C error rate too high");
            }

            id = FOC_GetId();
            iq = FOC_GetIq();

            DBG_Printf("%7lu  %6.1f  %6.2f  %6.2f  %6.2f  %6lu  %u\r\n",
                       (unsigned long)now,
                       (double)rpm,
                       (double)id,
                       (double)iq,
                       (double)iq_cmd,
                       (unsigned long)err,
                       (unsigned)Board_DrvFault());
        }

        Board_DelayMs(5u);
    }
}
