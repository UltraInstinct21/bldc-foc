/**
 * @file    usb_console.c
 * @brief   USB CDC console + "dfu" reboot command. See usb_console.h.
 *
 * Ported from the bring-up firmware that was verified on the board
 * (varun_foc six_step, 2026-11-17).
 */
#include "usb_console.h"
#include "board.h"
#include "tusb.h"

#include <string.h>

#ifndef APP_NAME
#define APP_NAME            "app"
#endif

#define BOOTLOADER_MAGIC    0xB007DF11u
#define SYSMEM_BASE         0x1FFF0000u
#define USB_IRQ_PRIORITY    6u          /* below the ADC/DMA control loop (1) */

/* Survives NVIC_SystemReset(): the linker script keeps .noinit out of the
 * zeroed .bss. */
static volatile uint32_t s_boot_request __attribute__((section(".noinit")));

static uint8_t s_ready;
static char    s_line[64];
static uint8_t s_line_len;
static char    s_cmd[64];               /* last complete line for the app */
static volatile uint8_t s_cmd_ready;

void Usb_JumpToBootloaderIfRequested(void)
{
    uint32_t sp;
    uint32_t pc;

    if (s_boot_request != BOOTLOADER_MAGIC)
    {
        return;
    }
    s_boot_request = 0u;

    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    (void)RCC->APB2ENR;
    SYSCFG->MEMRMP = SYSCFG_MEMRMP_MEM_MODE_0;      /* system memory at 0 */
    SCB->VTOR = SYSMEM_BASE;

    sp = *(volatile uint32_t *)SYSMEM_BASE;
    pc = *(volatile uint32_t *)(SYSMEM_BASE + 4u);
    __set_MSP(sp);
    ((void (*)(void))pc)();
    for (;;) { }
}

void Usb_Init(void)
{
    RCC_OscInitTypeDef       osc = {0};
    RCC_PeriphCLKInitTypeDef clk = {0};

    /* HSI48 for USB (no crystal on this board). */
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
    osc.HSI48State     = RCC_HSI48_ON;
    osc.PLL.PLLState   = RCC_PLL_NONE;
    if (HAL_RCC_OscConfig(&osc) != HAL_OK)
    {
        return;                         /* no USB; UART console still works */
    }

    clk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    clk.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
    if (HAL_RCCEx_PeriphCLKConfig(&clk) != HAL_OK)
    {
        return;
    }

    /* CRS trims HSI48 to the USB start-of-frame. */
    __HAL_RCC_CRS_CLK_ENABLE();
    CRS->CFGR = (CRS->CFGR & ~CRS_CFGR_SYNCSRC) | CRS_CFGR_SYNCSRC_1;
    CRS->CR  |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;

    __HAL_RCC_USB_CLK_ENABLE();
    __HAL_RCC_USB_FORCE_RESET();        /* clean state if the bootloader used it */
    __HAL_RCC_USB_RELEASE_RESET();
    NVIC_ClearPendingIRQ(USB_HP_IRQn);
    NVIC_ClearPendingIRQ(USB_LP_IRQn);
    NVIC_ClearPendingIRQ(USBWakeUp_IRQn);

    HAL_NVIC_SetPriority(USB_HP_IRQn,    USB_IRQ_PRIORITY, 0u);
    HAL_NVIC_SetPriority(USB_LP_IRQn,    USB_IRQ_PRIORITY, 0u);
    HAL_NVIC_SetPriority(USBWakeUp_IRQn, USB_IRQ_PRIORITY, 0u);

    (void)tud_init(0);                  /* enables the USB IRQs itself */
    s_ready = 1u;
}

static void reboot_to_dfu(void)
{
    uint32_t t0;

    Board_BridgeOff();
    (void)Usb_Write("\r\nRebooting into the ST DFU bootloader...\r\n", 43);

    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < 100u)  /* let USB send the message */
    {
        tud_task();
    }

    s_boot_request = BOOTLOADER_MAGIC;
    NVIC_SystemReset();
}

static void handle_char(char c)
{
    if ((c == 0x03) || (c == 0x1B))     /* Ctrl-C, ESC */
    {
        Board_BridgeOff();
        Board_RequestStop();
        s_line_len = 0u;
        (void)Usb_Write("\r\n[stop] bridge off\r\n", 21);
        return;
    }

    if ((c == '\r') || (c == '\n'))
    {
        s_line[s_line_len] = '\0';
        if (strcmp(s_line, "dfu") == 0)
        {
            reboot_to_dfu();
        }
        if (s_line_len != 0u)
        {
            (void)Usb_Write("\r\n", 2);
            if (s_cmd_ready == 0u)
            {
                memcpy(s_cmd, s_line, (uint32_t)s_line_len + 1u);
                s_cmd_ready = 1u;
            }
        }
        s_line_len = 0u;
        return;
    }

    if ((c >= ' ') && (c <= '~') && (s_line_len < (sizeof(s_line) - 1u)))
    {
        s_line[s_line_len++] = c;
        (void)Usb_Write(&c, 1);         /* echo */
    }
}

void Usb_Poll(void)
{
    if ((s_ready == 0u) || (__get_IPSR() != 0u))
    {
        return;
    }

    tud_task();

    /* Output printed before the terminal was opened went to the UART, so
     * say which app is running when a host opens the port. */
    {
        static uint8_t was_connected;
        uint8_t now = Usb_Connected();

        if ((now != 0u) && (was_connected == 0u))
        {
            static const char hello[] =
                "\r\n[console] STM32G431 " APP_NAME ", built " __DATE__ " " __TIME__
                ". 'dfu' = bootloader, Ctrl-C/ESC = bridge off.\r\n";
            (void)Usb_Write(hello, (int)sizeof(hello) - 1);
        }
        was_connected = now;
    }

    while (tud_cdc_available() != 0u)
    {
        char buf[16];
        uint32_t n = tud_cdc_read(buf, sizeof(buf));

        for (uint32_t i = 0u; i < n; i++)
        {
            handle_char(buf[i]);
        }
    }
}

void Usb_WaitForHost(uint32_t mount_ms, uint32_t open_ms)
{
    uint32_t t0 = HAL_GetTick();

    if (s_ready == 0u)
    {
        return;
    }
    while (!tud_mounted() && ((HAL_GetTick() - t0) < mount_ms))
    {
        Usb_Poll();
    }
    if (!tud_mounted())
    {
        return;                         /* no USB host: UART console only */
    }
    t0 = HAL_GetTick();
    while ((Usb_Connected() == 0u) && ((HAL_GetTick() - t0) < open_ms))
    {
        Usb_Poll();
    }
}

uint8_t Usb_GetLine(char *buf, uint32_t size)
{
    if ((s_cmd_ready == 0u) || (buf == NULL) || (size == 0u))
    {
        return 0u;
    }
    strncpy(buf, s_cmd, size - 1u);
    buf[size - 1u] = '\0';
    s_cmd_ready = 0u;
    return 1u;
}

uint8_t Usb_Connected(void)
{
    return ((s_ready != 0u) && tud_cdc_connected()) ? 1u : 0u;
}

int Usb_Write(const char *buf, int len)
{
    int      left = len;
    uint32_t t0;

    /* TinyUSB's FIFOs are not shared with interrupt context here: output
     * from an ISR is dropped rather than risk corrupting the stack. */
    if ((Usb_Connected() == 0u) || (len <= 0) || (__get_IPSR() != 0u))
    {
        return 0;
    }

    t0 = HAL_GetTick();
    while ((left > 0) && ((HAL_GetTick() - t0) < 100u))
    {
        uint32_t w = tud_cdc_write(buf, (uint32_t)left);

        buf  += w;
        left -= (int)w;
        (void)tud_cdc_write_flush();
        if (left > 0)
        {
            tud_task();
        }
    }
    return len - left;
}

/* ---- USB interrupts ----------------------------------------------------- */
void USB_HP_IRQHandler(void)    { tud_int_handler(0); }
void USB_LP_IRQHandler(void)    { tud_int_handler(0); }
void USBWakeUp_IRQHandler(void) { tud_int_handler(0); }
