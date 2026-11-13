/**
 * @file    esp_link.h
 * @brief   Half-duplex USART1 frame protocol for the STM32 <-> ESP32 link.
 *
 * Frame format (little-endian payload, big-endian CRC):
 *   [0]   0xA5        sync byte 0
 *   [1]   0x5A        sync byte 1
 *   [2]   type        0x01=CMD (ESP->STM), 0x02=TLM (STM->ESP)
 *   [3]   seq         rolling sequence number
 *   [4]   len         payload length in bytes
 *   [5..5+len-1]      payload
 *   [5+len..6+len]    CRC-16/CCITT-FALSE over bytes [2..4+len]
 *
 * CMD payload (10 bytes):
 *   u8  flags         bit0=enable, bit1=clear_fault
 *   i16 iq_ma         q-axis current reference, mA
 *   i16 id_ma         d-axis current reference, mA
 *   u16 rpm_limit     RPM limit (0 = use app default)
 *   u8  reserved[3]
 *
 * TLM payload (20 bytes):
 *   u8  flags         bit0=enabled, bit1=fault, bit2=cal_valid,
 *                     bit3=link_ok, bit4=rpm_limited, bit5=enc_error
 *   u8  drv_fault     raw nFAULT state (1=fault)
 *   i16 id_ma         measured d-axis current, mA
 *   i16 iq_ma         measured q-axis current, mA
 *   i16 iq_ref_ma     commanded q-axis current, mA
 *   i16 rpm_x10       mechanical RPM * 10 (signed)
 *   u16 angle_raw     raw encoder angle 0..4095
 *   u32 uptime_ms     milliseconds since boot
 *   u16 enc_errors    cumulative encoder I2C errors
 *   u16 isr_loops     FOC ISR count / 1000
 */
#ifndef ESP_LINK_H
#define ESP_LINK_H

#include <stdint.h>

/* Frame constants */
#define ESPL_SYNC0      0xA5u
#define ESPL_SYNC1      0x5Au
#define ESPL_TYPE_CMD   0x01u
#define ESPL_TYPE_TLM   0x02u

#define ESPL_CMD_FLAG_ENABLE        (1u << 0)
#define ESPL_CMD_FLAG_CLEAR_FAULT   (1u << 1)

#define ESPL_TLM_FLAG_ENABLED       (1u << 0)
#define ESPL_TLM_FLAG_FAULT         (1u << 1)
#define ESPL_TLM_FLAG_CAL_VALID     (1u << 2)
#define ESPL_TLM_FLAG_LINK_OK       (1u << 3)
#define ESPL_TLM_FLAG_RPM_LIMITED   (1u << 4)
#define ESPL_TLM_FLAG_ENC_ERROR     (1u << 5)

#define ESPL_CMD_PAYLOAD_LEN    10u
#define ESPL_TLM_PAYLOAD_LEN    20u
#define ESPL_MAX_PAYLOAD_LEN    24u
#define ESPL_MAX_FRAME_LEN      (5u + ESPL_MAX_PAYLOAD_LEN + 2u)  /* 31 */

/* Command decoded from the ESP32. */
typedef struct {
    uint8_t  flags;
    int16_t  iq_ma;
    int16_t  id_ma;
    uint16_t rpm_limit;
} EspCmd;

/* Telemetry to send to the ESP32. */
typedef struct {
    uint8_t  flags;
    uint8_t  drv_fault;
    int16_t  id_ma;
    int16_t  iq_ma;
    int16_t  iq_ref_ma;
    int16_t  rpm_x10;
    uint16_t angle_raw;
    uint32_t uptime_ms;
    uint16_t enc_errors;
    uint16_t isr_loops;
} EspTlm;

/* Link statistics. */
typedef struct {
    uint32_t frames_ok;
    uint32_t crc_errors;
    uint32_t rx_overflow;
    uint32_t last_rx_ms;    /* Board_Millis() when last valid CMD arrived */
} EspLinkStats;

/**
 * Initialise USART1 as half-duplex at baud, start RX interrupt.
 * Call after Board_Init().
 */
void EspLink_Init(uint32_t baud);

/**
 * Call from the main loop. Returns 1 and fills *cmd if a new valid CMD
 * frame has arrived since the last call. Returns 0 otherwise.
 * Also fills *seq with the frame's sequence byte.
 */
int EspLink_TakeCmd(EspCmd *cmd, uint8_t *seq);

/**
 * Send a TLM frame with the given payload and sequence number.
 * Blocking (HAL_UART_Transmit). Echo received on the half-duplex pin
 * will be parsed and safely ignored by the parser.
 */
void EspLink_SendTlm(const EspTlm *tlm, uint8_t seq);

/** Fill *s with a copy of current statistics. */
void EspLink_GetStats(EspLinkStats *s);

/** True if a valid CMD was received within timeout_ms. */
uint8_t EspLink_IsAlive(uint32_t timeout_ms);

#endif /* ESP_LINK_H */
