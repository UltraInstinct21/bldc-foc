/**
 * @file    debug.c
 * @brief   Blocking UART console. TX-only, so it never touches PB7.
 */
#include "debug.h"
#include "board.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define DBG_BUF_SIZE    192u
#define DBG_TIMEOUT_MS  100u

void DBG_Init(void)
{
    /* USART1 is configured by Board_Init(). */
}

void DBG_PutChar(char c)
{
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)&c, 1u, DBG_TIMEOUT_MS);
}

void DBG_Puts(const char *s)
{
    if (s == NULL)
    {
        return;
    }
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)s, (uint16_t)strlen(s),
                            DBG_TIMEOUT_MS);
}

void DBG_Printf(const char *fmt, ...)
{
    char    buf[DBG_BUF_SIZE];
    va_list ap;
    int     n;

    if (fmt == NULL)
    {
        return;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    if (n < 0)
    {
        return;
    }
    if ((unsigned)n >= sizeof(buf))
    {
        n = (int)sizeof(buf) - 1;
    }

    (void)HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)n,
                            DBG_TIMEOUT_MS);
}
