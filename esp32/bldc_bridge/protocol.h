#pragma once
#include <Arduino.h>

// ---- Frame constants -------------------------------------------------------
#define ESPL_SYNC0        0xA5
#define ESPL_SYNC1        0x5A
#define ESPL_TYPE_CMD     0x01
#define ESPL_TYPE_TLM     0x02

#define ESPL_CMD_FLAG_ENABLE       (1 << 0)
#define ESPL_CMD_FLAG_CLEAR_FAULT  (1 << 1)

#define ESPL_TLM_FLAG_ENABLED      (1 << 0)
#define ESPL_TLM_FLAG_FAULT        (1 << 1)
#define ESPL_TLM_FLAG_CAL_VALID    (1 << 2)
#define ESPL_TLM_FLAG_LINK_OK      (1 << 3)
#define ESPL_TLM_FLAG_RPM_LIMITED  (1 << 4)
#define ESPL_TLM_FLAG_ENC_ERROR    (1 << 5)

#define ESPL_CMD_PAYLOAD_LEN    10
#define ESPL_TLM_PAYLOAD_LEN    20
#define ESPL_MAX_FRAME_LEN      31

// ---- Decoded telemetry -----------------------------------------------------
struct Telemetry {
  bool    valid;
  uint8_t flags;
  uint8_t drv_fault;
  float   id_a;
  float   iq_a;
  float   iq_ref_a;
  float   rpm;
  int     angle_raw;
  uint32_t uptime_ms;
  uint16_t enc_errors;
  uint16_t isr_loops;
  uint32_t rx_time_ms;
};

// ---- CRC-16/CCITT-FALSE: init=0xFFFF, poly=0x1021, no reflection ----------
inline uint16_t crc16_update(uint16_t crc, uint8_t b) {
  crc ^= (uint16_t)b << 8;
  for (int i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
  return crc;
}

inline uint16_t crc16(const uint8_t *buf, int len) {
  uint16_t crc = 0xFFFF;
  for (int i = 0; i < len; i++) crc = crc16_update(crc, buf[i]);
  return crc;
}

// ---- Build a CMD frame into out[], return frame length ---------------------
inline int buildCmd(uint8_t *out, uint8_t seq,
                    bool enable, bool clear_fault,
                    int16_t iq_ma, int16_t id_ma, uint16_t rpm_limit) {
  uint8_t pl[ESPL_CMD_PAYLOAD_LEN] = {};
  pl[0] = (enable ? ESPL_CMD_FLAG_ENABLE : 0) |
           (clear_fault ? ESPL_CMD_FLAG_CLEAR_FAULT : 0);
  pl[1] = (uint8_t)(iq_ma & 0xFF);
  pl[2] = (uint8_t)(iq_ma >> 8);
  pl[3] = (uint8_t)(id_ma & 0xFF);
  pl[4] = (uint8_t)(id_ma >> 8);
  pl[5] = (uint8_t)(rpm_limit & 0xFF);
  pl[6] = (uint8_t)(rpm_limit >> 8);

  out[0] = ESPL_SYNC0;
  out[1] = ESPL_SYNC1;
  out[2] = ESPL_TYPE_CMD;
  out[3] = seq;
  out[4] = ESPL_CMD_PAYLOAD_LEN;
  memcpy(&out[5], pl, ESPL_CMD_PAYLOAD_LEN);

  uint16_t c = crc16(&out[2], 3 + ESPL_CMD_PAYLOAD_LEN);
  int flen   = 5 + ESPL_CMD_PAYLOAD_LEN;
  out[flen]     = (uint8_t)(c >> 8);
  out[flen + 1] = (uint8_t)(c & 0xFF);
  return flen + 2;
}

// ---- Parse TLM payload into Telemetry struct (pl must be TLM_PAYLOAD_LEN) -
inline void parseTlm(const uint8_t *pl, Telemetry &t) {
  t.flags      = pl[0];
  t.drv_fault  = pl[1];
  t.id_a       = (int16_t)(pl[2] | ((uint16_t)pl[3] << 8)) / 1000.0f;
  t.iq_a       = (int16_t)(pl[4] | ((uint16_t)pl[5] << 8)) / 1000.0f;
  t.iq_ref_a   = (int16_t)(pl[6] | ((uint16_t)pl[7] << 8)) / 1000.0f;
  t.rpm        = (int16_t)(pl[8] | ((uint16_t)pl[9] << 8)) / 10.0f;
  t.angle_raw  = pl[10] | ((uint16_t)pl[11] << 8);
  t.uptime_ms  = (uint32_t)pl[12] | ((uint32_t)pl[13] << 8) |
                 ((uint32_t)pl[14] << 16) | ((uint32_t)pl[15] << 24);
  t.enc_errors = pl[16] | ((uint16_t)pl[17] << 8);
  t.isr_loops  = pl[18] | ((uint16_t)pl[19] << 8);
  t.rx_time_ms = millis();
  t.valid      = true;
}
