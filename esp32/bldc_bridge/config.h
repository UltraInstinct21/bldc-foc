#pragma once

// WiFi – AP mode (the ESP32 creates its own network)
#define WIFI_SSID       "BLDC-FOC"
#define WIFI_PASS       "bldcfoc123"   // change before use

// UART to STM32 (half-duplex single-wire on Serial2)
// Wire: GPIO17 TX -> 1k -> PB6, GPIO16 RX -> PB6 (same node), 4.7k PU to 3V3.
#define STM_UART_RX     16
#define STM_UART_TX     17
#define STM_UART_BAUD   230400

// The angle sample is transported with every command. Keep this at 5 ms or
// slower: a command plus telemetry reply takes about 2 ms at 230400 baud.
#define POLL_INTERVAL_MS    5       // 200 Hz

// AS5600 OUT -> this ESP32 ADC pin. GPIO34 is input-only and ADC1-backed,
// so WiFi activity cannot interfere with the conversion peripheral.
#define AS5600_ADC_ENABLED 0
#define AS5600_ADC_PIN      34
#define AS5600_ADC_SAMPLES  4
#define AS5600_ADC_FULL_SCALE_MV 3300
// GPIO34 ADC input uses a 10k/100k divider so AS5600's 0..3.3 V output
// remains inside the ESP32 ADC's usable range.
#define AS5600_ADC_DIVIDER_TOP_KOHM    10
#define AS5600_ADC_DIVIDER_BOTTOM_KOHM 100

// If no web heartbeat for this long, zero iq and disable.
#define WEB_DEADMAN_MS      2000
