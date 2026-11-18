/**
 * @file    usb_console.h
 * @brief   USB CDC console on the USB-C connector (PA11/PA12), TinyUSB.
 *
 * The board has no crystal, so USB runs from HSI48 trimmed by the CRS to
 * the host's start-of-frame (verified on the board).
 *
 * Console routing (see _write() in board.c):
 *   - a host has the USB serial port open  -> output goes to USB
 *   - otherwise                            -> output goes to USART1 TX (PB6, J7.1)
 *
 * Commands understood in every app (type them into the USB serial port):
 *   dfu            stop the bridge and reboot into the ST ROM DFU bootloader,
 *                  so the next flash needs no BOOT0 jumper (tools/flash.py
 *                  sends this automatically)
 *   Ctrl-C / ESC   immediate bridge off (MOE = 0); see Board_StopRequested()
 *   echo off|on    stop/resume echoing typed characters (for host programs
 *                  such as the UI backend; not passed to the app)
 *
 * Up to 4 complete lines are queued for Usb_GetLine(), so a host program may
 * send commands back to back.
 */
#ifndef USB_CONSOLE_H
#define USB_CONSOLE_H

#include <stdint.h>

/* Call first thing in main(), before any clock setup. Jumps to the ROM
 * bootloader if the previous run asked for it with the "dfu" command. */
void    Usb_JumpToBootloaderIfRequested(void);

/* HSI48 + CRS, USB clock, TinyUSB device stack. Call after the system clock. */
void    Usb_Init(void);

/* Run the USB stack and handle console input. Call often from thread mode;
 * Board_DelayMs()/HAL_Delay() already do. Does nothing in interrupt context. */
void    Usb_Poll(void);

/* Boot helper: wait up to mount_ms for USB enumeration, then (only if a
 * host enumerated us) up to open_ms for a terminal to open the port, so
 * one-shot boot output lands on USB instead of the UART. */
void    Usb_WaitForHost(uint32_t mount_ms, uint32_t open_ms);

/* Fetch the next complete console line typed by the host (not "dfu").
 * Returns 1 and copies it (NUL-terminated) if one is waiting. Lines are
 * handed over here instead of being executed inside Usb_Poll(), so app
 * commands may block or call Board_DelayMs() safely. */
uint8_t Usb_GetLine(char *buf, uint32_t size);

/* 1 if a host has the CDC port open (DTR set). */
uint8_t Usb_Connected(void);

/* Blocking write with a 100 ms stall timeout. Returns bytes accepted. */
int     Usb_Write(const char *buf, int len);

#endif /* USB_CONSOLE_H */
