/**
 * @file    esp_link.c
 * @brief   Half-duplex USART1 link to the ESP32.
 *
 * Frame parsing runs in interrupt context; commands are consumed in the
 * main loop. In HDSEL mode, outgoing transmissions generate local RX echos;
 * the parser safely ignores frames where type != ESPL_TYPE_CMD.
 */
#include "esp_link.h"
#include "board.h"
#include <string.h>

/* --------------------------------------------------------------------------
 * CRC-16/CCITT-FALSE: init=0xFFFF, poly=0x1021, input/output not reflected.
 * Coverage: type, seq, len, payload bytes.
 * -------------------------------------------------------------------------- */
static uint16_t crc16(const uint8_t *buf, uint32_t n)
{
    uint16_t crc = 0xFFFFu;
    for (uint32_t i = 0u; i < n; i++)
    {
        crc ^= (uint16_t)buf[i] << 8;
        for (int b = 0; b < 8; b++)
        {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
                                  : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* --------------------------------------------------------------------------
 * RX state machine
 * -------------------------------------------------------------------------- */
typedef enum {
    ST_SYNC0 = 0,
    ST_SYNC1,
    ST_TYPE,
    ST_SEQ,
    ST_LEN,
    ST_PAYLOAD,
    ST_CRC0,
    ST_CRC1
} RxState;

static struct {
    RxState state;
    uint8_t hdr[3];                         /* type, seq, len */
    uint8_t buf[ESPL_MAX_PAYLOAD_LEN + 2u]; /* payload + crc scratch */
    uint8_t pos;
    uint8_t crc_hi;
} s_rx;

/* Pending decoded command consumed in main loop. */
static volatile struct {
    EspCmd  cmd;
    uint8_t seq;
    uint8_t ready;
} s_pend;

static EspLinkStats s_stats;
static volatile uint8_t s_rx_byte;
static uint8_t s_tx_buf[ESPL_MAX_FRAME_LEN];

static void on_byte(uint8_t b)
{
    switch (s_rx.state)
    {
    case ST_SYNC0:
        if (b == ESPL_SYNC0) { s_rx.state = ST_SYNC1; }
        break;

    case ST_SYNC1:
        s_rx.state = (b == ESPL_SYNC1) ? ST_TYPE : ST_SYNC0;
        break;

    case ST_TYPE:
        s_rx.hdr[0] = b;
        s_rx.state  = ST_SEQ;
        break;

    case ST_SEQ:
        s_rx.hdr[1] = b;
        s_rx.state  = ST_LEN;
        break;

    case ST_LEN:
        if (b > ESPL_MAX_PAYLOAD_LEN)
        {
            s_stats.rx_overflow++;
            s_rx.state = ST_SYNC0;
        }
        else
        {
            s_rx.hdr[2] = b;
            s_rx.pos    = 0u;
            s_rx.state  = (b == 0u) ? ST_CRC0 : ST_PAYLOAD;
        }
        break;

    case ST_PAYLOAD:
        s_rx.buf[s_rx.pos++] = b;
        if (s_rx.pos >= s_rx.hdr[2])
        {
            s_rx.state = ST_CRC0;
        }
        break;

    case ST_CRC0:
        s_rx.crc_hi = b;
        s_rx.state  = ST_CRC1;
        break;

    case ST_CRC1:
    {
        uint16_t recv_crc = ((uint16_t)s_rx.crc_hi << 8) | b;
        uint8_t  chk[3 + ESPL_MAX_PAYLOAD_LEN];
        memcpy(&chk[0], s_rx.hdr, 3);
        if (s_rx.hdr[2] > 0u)
        {
            memcpy(&chk[3], s_rx.buf, s_rx.hdr[2]);
        }
        uint16_t calc_crc = crc16(chk, 3u + s_rx.hdr[2]);

        if (calc_crc == recv_crc)
        {
            s_stats.frames_ok++;
            s_stats.last_rx_ms = Board_Millis();

            /* Accept CMD frames only (discards own TLM echo). */
            if ((s_rx.hdr[0] == ESPL_TYPE_CMD) && (s_rx.hdr[2] == ESPL_CMD_PAYLOAD_LEN))
            {
                EspCmd c;
                c.flags     = s_rx.buf[0];
                c.iq_ma     = (int16_t)((uint16_t)s_rx.buf[1] | ((uint16_t)s_rx.buf[2] << 8));
                c.id_ma     = (int16_t)((uint16_t)s_rx.buf[3] | ((uint16_t)s_rx.buf[4] << 8));
                c.rpm_limit = (uint16_t)s_rx.buf[5] | ((uint16_t)s_rx.buf[6] << 8);

                s_pend.cmd   = c;
                s_pend.seq   = s_rx.hdr[1];
                s_pend.ready = 1u;
            }
        }
        else
        {
            s_stats.crc_errors++;
        }

        s_rx.state = ST_SYNC0;
        break;
    }

    default:
        s_rx.state = ST_SYNC0;
        break;
    }
}

/* --------------------------------------------------------------------------
 * HAL UART callbacks
 * -------------------------------------------------------------------------- */
void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        on_byte(s_rx_byte);
        (void)HAL_UART_Receive_IT(&huart1, (uint8_t *)&s_rx_byte, 1u);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART1)
    {
        __HAL_UART_CLEAR_OREFLAG(huart);
        __HAL_UART_CLEAR_FEFLAG(huart);
        __HAL_UART_CLEAR_NEFLAG(huart);
        (void)HAL_UART_Receive_IT(&huart1, (uint8_t *)&s_rx_byte, 1u);
    }
}

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
void EspLink_Init(uint32_t baud)
{
    memset(&s_rx, 0, sizeof(s_rx));
    memset(&s_stats, 0, sizeof(s_stats));
    s_pend.ready = 0u;

    Board_InitUsart1Link(baud);
    (void)HAL_UART_Receive_IT(&huart1, (uint8_t *)&s_rx_byte, 1u);
}

int EspLink_TakeCmd(EspCmd *cmd, uint8_t *seq)
{
    if (!s_pend.ready)
    {
        return 0;
    }

    __disable_irq();
    *cmd = s_pend.cmd;
    *seq = s_pend.seq;
    s_pend.ready = 0u;
    __enable_irq();
    return 1;
}

void EspLink_SendTlm(const EspTlm *t, uint8_t seq)
{
    uint8_t  pl[ESPL_TLM_PAYLOAD_LEN];
    uint16_t crc;
    uint32_t len;

    /* Pack payload (little-endian) */
    pl[0]  = t->flags;
    pl[1]  = t->drv_fault;
    pl[2]  = (uint8_t)(t->id_ma & 0xFF);
    pl[3]  = (uint8_t)((uint16_t)t->id_ma >> 8);
    pl[4]  = (uint8_t)(t->iq_ma & 0xFF);
    pl[5]  = (uint8_t)((uint16_t)t->iq_ma >> 8);
    pl[6]  = (uint8_t)(t->iq_ref_ma & 0xFF);
    pl[7]  = (uint8_t)((uint16_t)t->iq_ref_ma >> 8);
    pl[8]  = (uint8_t)(t->rpm_x10 & 0xFF);
    pl[9]  = (uint8_t)((uint16_t)t->rpm_x10 >> 8);
    pl[10] = (uint8_t)(t->angle_raw & 0xFF);
    pl[11] = (uint8_t)(t->angle_raw >> 8);
    pl[12] = (uint8_t)(t->uptime_ms & 0xFF);
    pl[13] = (uint8_t)((t->uptime_ms >> 8) & 0xFF);
    pl[14] = (uint8_t)((t->uptime_ms >> 16) & 0xFF);
    pl[15] = (uint8_t)((t->uptime_ms >> 24) & 0xFF);
    pl[16] = (uint8_t)(t->enc_errors & 0xFF);
    pl[17] = (uint8_t)(t->enc_errors >> 8);
    pl[18] = (uint8_t)(t->isr_loops & 0xFF);
    pl[19] = (uint8_t)(t->isr_loops >> 8);

    /* Frame construction */
    s_tx_buf[0] = ESPL_SYNC0;
    s_tx_buf[1] = ESPL_SYNC1;
    s_tx_buf[2] = ESPL_TYPE_TLM;
    s_tx_buf[3] = seq;
    s_tx_buf[4] = ESPL_TLM_PAYLOAD_LEN;
    memcpy(&s_tx_buf[5], pl, ESPL_TLM_PAYLOAD_LEN);

    /* CRC over [2..4+len] */
    crc = crc16(&s_tx_buf[2], 3u + ESPL_TLM_PAYLOAD_LEN);
    len = 5u + ESPL_TLM_PAYLOAD_LEN;
    s_tx_buf[len]      = (uint8_t)(crc >> 8);
    s_tx_buf[len + 1u] = (uint8_t)(crc & 0xFF);
    len += 2u;

    (void)HAL_UART_Transmit(&huart1, s_tx_buf, (uint16_t)len, 10u);
}

void EspLink_GetStats(EspLinkStats *s)
{
    __disable_irq();
    *s = s_stats;
    __enable_irq();
}

uint8_t EspLink_IsAlive(uint32_t timeout_ms)
{
    if (s_stats.last_rx_ms == 0u)
    {
        return 0u;
    }
    return ((Board_Millis() - s_stats.last_rx_ms) < timeout_ms) ? 1u : 0u;
}
