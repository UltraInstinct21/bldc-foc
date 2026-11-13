#pragma once

// WiFi – AP mode (the ESP32 creates its own network)
#define WIFI_SSID       "BLDC-FOC"
#define WIFI_PASS       "bldcfoc123"   // change before use

// UART to STM32 (half-duplex single-wire on Serial2)
// Wire: GPIO17 TX -> 1k -> PB6, GPIO16 RX -> PB6 (same node), 4.7k PU to 3V3.
#define STM_UART_RX     16
#define STM_UART_TX     17
#define STM_UART_BAUD   230400

// Poll the STM32 at this rate (ms). Keep >= 20 to avoid collisions.
#define POLL_INTERVAL_MS    20      // 50 Hz

// If no web heartbeat for this long, zero iq and disable.
#define WEB_DEADMAN_MS      2000
