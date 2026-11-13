/**
 * @file    debug.h
 * @brief   Console I/O redirected to standard stdio (printf/fputs/putchar).
 */
#ifndef DEBUG_H
#define DEBUG_H

#include <stdio.h>

#define DBG_Init()          ((void)0)
#define DBG_Printf          printf
#define DBG_Puts(s)         fputs((s), stdout)
#define DBG_PutChar(c)      putchar(c)

#endif /* DEBUG_H */
