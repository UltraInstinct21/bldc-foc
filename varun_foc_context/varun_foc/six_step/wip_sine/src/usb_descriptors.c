#include <string.h>
#include "stm32g4xx.h"
#include "tusb.h"

// 0xCafe is TinyUSB's test VID -- fine for personal use, not for products.
static tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200,
  .bDeviceClass       = TUSB_CLASS_MISC,
  .bDeviceSubClass    = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol    = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = 0xCafe,
  .idProduct          = 0x4001,
  .bcdDevice          = 0x0100,
  .iManufacturer      = 1,
  .iProduct           = 2,
  .iSerialNumber      = 3,
  .bNumConfigurations = 1,
};

uint8_t const *tud_descriptor_device_cb(void) {
  return (uint8_t const *)&desc_device;
}

enum { ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA, ITF_NUM_TOTAL };

#define EPNUM_CDC_NOTIF   0x81
#define EPNUM_CDC_OUT     0x02
#define EPNUM_CDC_IN      0x82
#define CONFIG_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN)

static uint8_t const desc_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, 0x00, 100),
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
  (void)index;
  return desc_configuration;
}

static char const *string_desc_arr[] = {
  NULL,                 // 0: language (handled below)
  "Custom",             // 1: manufacturer
  "STM32G431 6step",    // 2: product
  NULL,                 // 3: serial (from chip unique ID)
  "6step CDC",          // 4: CDC interface
};

static uint16_t desc_str[33];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
  (void)langid;
  uint8_t n = 0;

  if (index == 0) {
    desc_str[1] = 0x0409;  // English (US)
    n = 1;
  } else if (index == 3) {
    uint32_t const *uid = (uint32_t const *)UID_BASE;
    for (int w = 0; w < 3; w++) {
      uint32_t v = uid[w];
      for (int i = 0; i < 8; i++, v <<= 4) {
        uint8_t nib = (v >> 28) & 0xF;
        desc_str[1 + n++] = nib < 10 ? '0' + nib : 'A' + nib - 10;
      }
    }
  } else {
    if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0])) return NULL;
    char const *s = string_desc_arr[index];
    n = (uint8_t)strlen(s);
    if (n > 32) n = 32;
    for (uint8_t i = 0; i < n; i++) desc_str[1 + i] = s[i];
  }

  desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * n + 2));
  return desc_str;
}
