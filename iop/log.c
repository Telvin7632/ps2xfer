/*
 * Log ring buffer (16 KiB). Never used on the per-packet path.
 * Readers use an absolute byte cursor; if they fall behind by more than the
 * ring size, the lost byte count is reported.
 */
#include <stdarg.h>
#ifdef _IOP
#include "irx_imports.h"
#else
#include <stdio.h>
#include <string.h>
#endif
#include "px.h"

#define LOG_SIZE 16384 /* power of two */

static char g_log[LOG_SIZE];
static u32 g_log_head; /* total bytes ever written */

static void log_putc(char c)
{
    g_log[g_log_head & (LOG_SIZE - 1)] = c;
    g_log_head++;
}

void px_log_init(void)
{
    g_log_head = 0;
}

#ifdef _IOP
static void prnt_cb(void *ctx, int c)
{
    (void)ctx;
    if (c > 0)
        log_putc((char)c);
}
#endif

void px_log(const char *fmt, ...)
{
    va_list ap;
    int st;
    char stamp[16];
    u32 ms = px_ms();
    const char *p;

    /* "[sssss.mmm] " prefix without printf-into-ring re-entrancy issues */
    sprintf(stamp, "[%5u.%03u] ", (unsigned)(ms / 1000), (unsigned)(ms % 1000));

    st = plat_lock();
    for (p = stamp; *p; p++)
        log_putc(*p);
    va_start(ap, fmt);
#ifdef _IOP
    prnt((print_callback_t)prnt_cb, NULL, fmt, ap);
#else
    {
        char tmp[256];
        int n = vsnprintf(tmp, sizeof(tmp), fmt, ap), i;
        if (n > (int)sizeof(tmp) - 1)
            n = (int)sizeof(tmp) - 1;
        for (i = 0; i < n; i++)
            log_putc(tmp[i]);
    }
#endif
    va_end(ap);
    plat_unlock(st);
}

u32 px_log_read(u32 *cursor, char *dst, u32 max, u32 *lost)
{
    u32 n = 0, c;
    int st = plat_lock();

    c = *cursor;
    *lost = 0;
    if (g_log_head - c > LOG_SIZE) {
        *lost = g_log_head - c - LOG_SIZE;
        c = g_log_head - LOG_SIZE;
    }
    while (c != g_log_head && n < max) {
        dst[n++] = g_log[c & (LOG_SIZE - 1)];
        c++;
    }
    *cursor = c;
    plat_unlock(st);
    return n;
}
