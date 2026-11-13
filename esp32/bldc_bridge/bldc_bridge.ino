/**
 * bldc_bridge.ino — ESP32 WiFi bridge for the BLDC-FOC controller.
 *
 * Hardware: any ESP32 (WROOM-32, DevKitC, etc.) powered from USB.
 *
 * UART wiring to STM32:
 *   ESP32 GPIO17 (UART2 TX) -> 1 kΩ -> PB6 (J7.1 on the BLDC board)
 *   ESP32 GPIO16 (UART2 RX) -> PB6 (same node, direct)
 *   Add a 4.7 kΩ pull-up from the PB6 node to 3.3 V.
 *   ESP32 GND -> J6.3 (board GND). Common ground is essential.
 *   Power the ESP32 from its own USB port.
 *
 * Web interface: connect to WiFi "BLDC-FOC" (password: bldcfoc123),
 * then browse to http://192.168.4.1
 *
 * Libraries: none beyond the standard ESP32 Arduino core (WiFi, WebServer).
 */

#include <WiFi.h>
#include <WebServer.h>
#include "config.h"
#include "protocol.h"
#include "web_page.h"

// ---- state -----------------------------------------------------------------
static bool     g_enable       = false;
static bool     g_clear_fault  = false;
static int16_t  g_iq_ma        = 0;
static int16_t  g_id_ma        = 0;
static uint16_t g_rpm_limit    = 400;

static uint8_t    g_seq         = 0;
static Telemetry  g_tlm         = {};
static uint32_t   g_frames_ok   = 0;
static uint32_t   g_crc_errors  = 0;
static uint32_t   g_last_web_hb = 0;   // last /api/set time
static uint32_t   g_last_poll   = 0;

// ---- WebServer -------------------------------------------------------------
static WebServer server(80);

// ---- RX parser state -------------------------------------------------------
typedef enum { ST_SYNC0=0, ST_SYNC1, ST_TYPE, ST_SEQ, ST_LEN, ST_PAYLOAD, ST_CRC0, ST_CRC1 } RxSt;
static struct {
  RxSt    state = ST_SYNC0;
  uint8_t type, seq, len, pos;
  uint8_t hdr[3];
  uint8_t buf[ESPL_MAX_FRAME_LEN];
} rx;

static void rx_feed(uint8_t b) {
  switch(rx.state) {
    case ST_SYNC0:  if(b==ESPL_SYNC0) rx.state=ST_SYNC1; break;
    case ST_SYNC1:  rx.state = (b==ESPL_SYNC1)?ST_TYPE:ST_SYNC0; break;
    case ST_TYPE:   rx.hdr[0]=rx.type=b; rx.state=ST_SEQ; break;
    case ST_SEQ:    rx.hdr[1]=rx.seq=b;  rx.state=ST_LEN; break;
    case ST_LEN:
      if(b>ESPL_TLM_PAYLOAD_LEN) { rx.state=ST_SYNC0; break; }
      rx.hdr[2]=rx.len=b; rx.pos=0;
      rx.state = (b==0)?ST_CRC0:ST_PAYLOAD; break;
    case ST_PAYLOAD:
      rx.buf[rx.pos++]=b;
      if(rx.pos>=rx.len) rx.state=ST_CRC0; break;
    case ST_CRC0:
      rx.buf[ESPL_TLM_PAYLOAD_LEN]=b; rx.state=ST_CRC1; break;
    case ST_CRC1: {
      uint16_t recv = ((uint16_t)rx.buf[ESPL_TLM_PAYLOAD_LEN]<<8)|b;
      uint8_t tmp[3 + ESPL_TLM_PAYLOAD_LEN];
      memcpy(tmp, rx.hdr, 3);
      memcpy(tmp+3, rx.buf, rx.len);
      uint16_t calc = crc16(tmp, 3 + rx.len);
      if(calc == recv) {
        g_frames_ok++;
        // Only accept TLM frames (ignore own CMD echoes)
        if(rx.type == ESPL_TYPE_TLM && rx.len == ESPL_TLM_PAYLOAD_LEN) {
          parseTlm(rx.buf, g_tlm);
        }
      } else {
        g_crc_errors++;
      }
      rx.state = ST_SYNC0;
      break;
    }
  }
}

// ---- UART poll (called in loop) -------------------------------------------
static void stm_poll() {
  if(millis() - g_last_poll < POLL_INTERVAL_MS) return;
  g_last_poll = millis();

  // Web deadman: if browser hasn't sent a command recently, disable
  if(g_enable && (millis() - g_last_web_hb) > WEB_DEADMAN_MS) {
    g_enable  = false;
    g_iq_ma   = 0;
    Serial.println("[deadman] web timeout, disabling");
  }

  uint8_t frame[ESPL_MAX_FRAME_LEN];
  int flen = buildCmd(frame, g_seq++, g_enable, g_clear_fault, g_iq_ma, g_id_ma, g_rpm_limit);
  g_clear_fault = false; // one-shot

  Serial2.write(frame, flen);
  Serial2.flush();

  // Read incoming bytes for ~15 ms (wait for TLM reply)
  uint32_t t0 = millis();
  while(millis() - t0 < 15) {
    while(Serial2.available()) rx_feed(Serial2.read());
    delay(1);
  }
}

// ---- HTTP handlers ---------------------------------------------------------
static void handleRoot() {
  server.sendHeader("Content-Encoding", "identity");
  server.send_P(200, "text/html", WEB_PAGE);
}

static void handleState() {
  uint32_t age = g_tlm.valid ? (millis() - g_tlm.rx_time_ms) : 99999;
  char buf[512];
  snprintf(buf, sizeof(buf),
    "{\"link_age_ms\":%lu,\"flags\":%u,\"drv_fault\":%u,"
    "\"id_a\":%.3f,\"iq_a\":%.3f,\"iq_ref_a\":%.3f,"
    "\"rpm\":%.1f,\"angle_raw\":%d,"
    "\"uptime_ms\":%lu,\"enc_errors\":%u,\"isr_loops\":%u,"
    "\"frames_ok\":%lu,\"crc_errors\":%lu}",
    (unsigned long)age,
    g_tlm.flags, g_tlm.drv_fault,
    g_tlm.id_a, g_tlm.iq_a, g_tlm.iq_ref_a,
    g_tlm.rpm, g_tlm.angle_raw,
    (unsigned long)g_tlm.uptime_ms,
    g_tlm.enc_errors, g_tlm.isr_loops,
    (unsigned long)g_frames_ok, (unsigned long)g_crc_errors);
  server.send(200, "application/json", buf);
}

static void handleSet() {
  if(server.hasArg("en"))  g_enable = server.arg("en").toInt() != 0;
  if(server.hasArg("iq"))  g_iq_ma  = (int16_t)constrain(server.arg("iq").toInt(), -4000, 4000);
  if(server.hasArg("id"))  g_id_ma  = (int16_t)server.arg("id").toInt();
  if(server.hasArg("rpm")) g_rpm_limit = (uint16_t)constrain(server.arg("rpm").toInt(), 0, 2000);
  if(server.hasArg("cf") && server.arg("cf").toInt()) g_clear_fault = true;
  g_last_web_hb = millis();
  server.send(200, "application/json", "{\"ok\":true}");
}

static void handleEstop() {
  g_enable  = false;
  g_iq_ma   = 0;
  g_last_web_hb = millis();
  server.send(200, "application/json", "{\"ok\":true}");
}

// ---- setup / loop ----------------------------------------------------------
void setup() {
  Serial.begin(115200);
  Serial.println("\r\n=== BLDC-FOC ESP32 Bridge ===");

  Serial2.begin(STM_UART_BAUD, SERIAL_8N1, STM_UART_RX, STM_UART_TX);
  Serial.printf("UART2 at %d baud: RX=GPIO%d TX=GPIO%d\n",
                STM_UART_BAUD, STM_UART_RX, STM_UART_TX);

  WiFi.softAP(WIFI_SSID, WIFI_PASS);
  Serial.printf("WiFi AP: \"%s\"  IP: %s\n",
                WIFI_SSID, WiFi.softAPIP().toString().c_str());

  server.on("/",         HTTP_GET,  handleRoot);
  server.on("/api/state",HTTP_GET,  handleState);
  server.on("/api/set",  HTTP_GET,  handleSet);
  server.on("/api/estop",HTTP_GET,  handleEstop);
  server.begin();
  Serial.println("HTTP server started. Browse to http://192.168.4.1");
}

void loop() {
  server.handleClient();
  stm_poll();
}
