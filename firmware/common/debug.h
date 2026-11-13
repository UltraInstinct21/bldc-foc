/**
 * @file    debug.h
 * @brief   Tiny blocking UART console on USART1 TX (PB6 / J7.1).
 */
#ifndef DEBUG_H
#define DEBUG_H

#include <stdint.h>

/* Board_Init() already configures USART1. Kept for symmetry. */
void DBG_Init(void);

void DBG_PutChar(char c);
void DBG_Puts(const char *s);
void DBG_Printf(const char *fmt, ...);

#endif /* DEBUG_H */
