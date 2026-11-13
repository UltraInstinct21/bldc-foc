/**
 * @file    main.c
 * @brief   test_07_esp_link — ESP32 half-duplex USART1 link test.
 *
 * Tests the USART1 half-duplex link on PB6 (J7.1) without any motor.
 * The test sends a synthetic TLM frame every 100 ms with incrementing
 * counters, and echoes the iq_ma from any valid CMD it receives.
 *
 * Run this together with the ESP32 firmware and the web app to verify
 * the UART link before spinning the motor.
 *
 * Wiring: same as foc_esp (see docs/esp32.md).
 *
 * Expected behaviour with the ESP32 connected:
 *   - Web UI shows "ONLINE" and an incrementing counter in the angle field.
 *   - Commanding iq from the web UI is reflected in the iq_ref field.
 *   - RPM and id/iq show synthetic sine / ramp values.
 *
 * Flash with:
 *   make APP=test_07_esp_link flash
 */
#include "board.h"
#include "debug.h"
#include "esp_link.h"

#include <math.h>

#define LINK_BAUD           LINK_UART_BAUD   /* 230400 */
#define TLM_PERIOD_MS       100u

static float s_iq_echo = 0.0f;

int main(void)
{
    uint32_t t_tlm = 0u;
    uint32_t counter = 0u;
    float    theta = 0.0f;

    Board_Init();

    DBG_Puts("\r\n\r\n");
    DBG_Puts("=== test_07_esp_link ===\r\n");
    DBG_Printf("USART1 half-duplex on PB6 (J7.1), %lu baud\r\n",
               (unsigned long)LINK_BAUD);
    DBG_Puts("Sending synthetic TLM every 100 ms.\r\n");
    DBG_Puts("Wire ESP32 GPIO17->1k->PB6, GPIO16->PB6, 4.7k PU to 3.3V.\r\n\r\n");

    /* Switch USART1 to half-duplex link mode */
    EspLink_Init(LINK_BAUD);

    for (;;)
    {
        uint32_t now = Board_Millis();
        EspCmd   cmd;
        uint8_t  seq;
        EspTlm   tlm;

        /* Process any incoming command from the ESP32 */
        if (EspLink_TakeCmd(&cmd, &seq))
        {
            s_iq_echo = (float)cmd.iq_ma * 0.001f;

            /* Respond immediately with telemetry matching this sequence number */
            tlm.flags      = ESPL_TLM_FLAG_CAL_VALID | ESPL_TLM_FLAG_LINK_OK;
            if (cmd.flags & ESPL_CMD_FLAG_ENABLE)
            {
                tlm.flags |= ESPL_TLM_FLAG_ENABLED;
            }
            tlm.drv_fault  = 0u;
            tlm.id_ma      = 0;
            tlm.iq_ma      = (int16_t)(sinf(theta) * 500.0f);   /* synthetic +/-0.5 A */
            tlm.iq_ref_ma  = (int16_t)(s_iq_echo * 1000.0f);
            tlm.rpm_x10    = (int16_t)((counter % 800u) * 10u); /* synthetic 0..800 rpm */
            tlm.angle_raw  = (uint16_t)(counter % 4096u);
            tlm.uptime_ms  = now;
            tlm.enc_errors = 0u;
            tlm.isr_loops  = (uint16_t)(counter / 10u);

            EspLink_SendTlm(&tlm, seq);
        }

        /* Periodic synthetic transmission only when link is idle (no active CMD polling)
         * to prevent collisions on the single-wire half-duplex line */
        if (!EspLink_IsAlive(250u) && ((now - t_tlm) >= TLM_PERIOD_MS))
        {
            t_tlm = now;
            counter++;
            theta += 0.1f;
            if (theta > 6.283f)
            {
                theta -= 6.283f;
            }

            tlm.flags      = ESPL_TLM_FLAG_CAL_VALID;
            tlm.drv_fault  = 0u;
            tlm.id_ma      = 0;
            tlm.iq_ma      = (int16_t)(sinf(theta) * 500.0f);
            tlm.iq_ref_ma  = (int16_t)(s_iq_echo * 1000.0f);
            tlm.rpm_x10    = (int16_t)((counter % 800u) * 10u);
            tlm.angle_raw  = (uint16_t)(counter % 4096u);
            tlm.uptime_ms  = now;
            tlm.enc_errors = 0u;
            tlm.isr_loops  = (uint16_t)(counter / 10u);

            EspLink_SendTlm(&tlm, (uint8_t)(counter & 0xFFu));
        }

        Board_DelayMs(5u);
    }
}
