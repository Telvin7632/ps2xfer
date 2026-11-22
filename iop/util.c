/*
 * Config parsing, word-Fletcher checksum, time and error helpers.
 */
#ifdef _IOP
#include "irx_imports.h"
#else
#include <string.h>
#include <stdlib.h>
#endif
#include "px.h"

px_cfg_t g_cfg;
px_stats_t g_stats;

static u64 g_t0;

void px_cfg_defaults(px_cfg_t *c)
{
    memset(c, 0, sizeof(*c));
    c->ip         = (192u << 24) | (168u << 16) | (0u << 8) | 10u;
    c->port       = PX_DEFAULT_PORT;
    c->bufs       = 3;
    c->buf_units  = 8;
    c->ata_chunk  = 0;
    c->csum       = 16;
    c->ack_every  = 32;
    c->ack_ms     = 2;
    c->hdr_dma    = 0;
    c->dma_block  = 128;
    c->write_path = 0;
    c->pause      = 0;
    c->rx_prio    = 10;
    c->prof       = 0;
    c->state_mb   = 32;
    c->fs_iomanx  = 0;
}

int px_parse_ip(const char *s, u32 *ip)
{
    u32 v = 0, part;
    int i;

    for (i = 0; i < 4; i++) {
        if (*s < '0' || *s > '9')
            return -1;
        part = 0;
        while (*s >= '0' && *s <= '9') {
            part = part * 10 + (u32)(*s++ - '0');
            if (part > 255)
                return -1;
        }
        v = (v << 8) | part;
        if (i < 3) {
            if (*s != '.')
                return -1;
            s++;
        }
    }
    if (*s != '\0' && *s != ' ' && *s != '\t' && *s != '\r' && *s != '\n')
        return -1;
    *ip = v;
    return 0;
}

static int parse_u32(const char *s, u32 *out)
{
    u32 v = 0;

    if (*s < '0' || *s > '9')
        return -1;
    while (*s >= '0' && *s <= '9')
        v = v * 10 + (u32)(*s++ - '0');
    if (*s != '\0' && *s != '\r' && *s != '\n' && *s != ' ')
        return -1;
    *out = v;
    return 0;
}

struct cfg_key {
    const char *name;
    unsigned int off;
    u32 min, max;
    u8 live;
};

#define K(n, f, lo, hi, live) { n, (unsigned int)__builtin_offsetof(px_cfg_t, f), lo, hi, live }
static const struct cfg_key cfg_keys[] = {
    K("port", port, 1, 65535, 0),
    K("bufs", bufs, 2, PX_MAX_BUFS, 0),
    K("buf_units", buf_units, 1, PX_MAX_BUF_UNITS, 0),
    K("ata_chunk", ata_chunk, 0, 65535, 1),
    K("csum", csum, 0, 1000000, 1),
    K("ack_every", ack_every, 1, 512, 1),
    K("ack_ms", ack_ms, 1, 100, 1),
    K("hdr_dma", hdr_dma, 0, 1, 1),
    K("dma_block", dma_block, 0, 512, 1),
    K("write_path", write_path, 0, 1, 1),
    K("pause", pause, 0, 1, 0),
    K("rx_prio", rx_prio, 9, 120, 0),
    K("prof", prof, 0, 1, 1),
    K("state_mb", state_mb, 1, 4096, 1),
    K("fs_iomanx", fs_iomanx, 0, 1, 0),
    K("wr_boost", wr_boost, 0, 1, 1),
};

int px_cfg_set(px_cfg_t *c, const char *kv, int live)
{
    unsigned int i, n;
    u32 v;

    if (strncmp(kv, "ip=", 3) == 0)
        return live ? -1 : px_parse_ip(kv + 3, &c->ip);

    for (i = 0; i < sizeof(cfg_keys) / sizeof(cfg_keys[0]); i++) {
        n = (unsigned int)strlen(cfg_keys[i].name);
        if (strncmp(kv, cfg_keys[i].name, n) != 0 || kv[n] != '=')
            continue;
        if (live && !cfg_keys[i].live)
            return -1;
        if (parse_u32(kv + n + 1, &v) != 0 || v < cfg_keys[i].min || v > cfg_keys[i].max)
            return -1;
        if (cfg_keys[i].off == __builtin_offsetof(px_cfg_t, dma_block) &&
            v != 0 && v != 64 && v != 128 && v != 256 && v != 512)
            return -1;
        *(u32 *)((u8 *)c + cfg_keys[i].off) = v;
        return 0;
    }
    return -1;
}

/* word-Fletcher, see common/proto.h. Unrolled by 4; len is a multiple of 4. */
u32 px_fletcher(const void *buf, u32 len)
{
    const u32 *w = (const u32 *)buf;
    u32 n = len >> 2;
    u32 s1 = 0, s2 = 0;

    while (n >= 4) {
        s1 += w[0]; s2 += s1;
        s1 += w[1]; s2 += s1;
        s1 += w[2]; s2 += s1;
        s1 += w[3]; s2 += s1;
        w += 4;
        n -= 4;
    }
    while (n--) {
        s1 += *w++;
        s2 += s1;
    }
    return s2 ^ ((s1 << 16) | (s1 >> 16));
}

void px_fletcher_init(px_fletcher_t *f)
{
    f->s1 = f->s2 = 0;
}

/* same result as px_fletcher() over the concatenation of all updates */
void px_fletcher_update(px_fletcher_t *f, const void *buf, u32 len)
{
    const u32 *w = (const u32 *)buf;
    u32 n = len >> 2, s1 = f->s1, s2 = f->s2;

    /* 8 words per turn, loads first: no load-delay stalls and 3.25 instead
       of 5 instructions per word (verify on the IOP is bound by this loop) */
    while (n >= 8) {
        u32 a = w[0], b = w[1], c = w[2], d = w[3];
        s1 += a; s2 += s1;
        s1 += b; s2 += s1;
        a = w[4];
        b = w[5];
        s1 += c; s2 += s1;
        s1 += d; s2 += s1;
        c = w[6];
        d = w[7];
        s1 += a; s2 += s1;
        s1 += b; s2 += s1;
        s1 += c; s2 += s1;
        s1 += d; s2 += s1;
        w += 8;
        n -= 8;
    }
    while (n--) {
        s1 += *w++;
        s2 += s1;
    }
    f->s1 = s1;
    f->s2 = s2;
}

u32 px_fletcher_final(const px_fletcher_t *f)
{
    return f->s2 ^ ((f->s1 << 16) | (f->s1 >> 16));
}

static u32 g_idle_per_ms; /* idle loop increments per ms with nothing else running */

void px_idle_calibrate(u32 ms)
{
    u32 c0 = plat_idle_count();
    u64 t0 = plat_ticks(), t1;

    plat_sleep_us(ms * 1000);
    t1 = plat_ticks();
    g_idle_per_ms = (u32)((u64)(plat_idle_count() - c0) * plat_ticks_per_ms() / (t1 - t0 + 1));
    px_log("idle calibration: %u loops/ms\n", (unsigned)g_idle_per_ms);
}

u32 px_idle_x10(u32 count_delta, u64 ticks_delta)
{
    u64 expect;

    if (g_idle_per_ms == 0 || ticks_delta == 0)
        return 0;
    expect = (u64)g_idle_per_ms * ticks_delta / plat_ticks_per_ms();
    if (expect == 0)
        return 0;
    if ((u64)count_delta >= expect)
        return 1000;
    return (u32)((u64)count_delta * 1000 / expect);
}

void px_load_update(void)
{
    static u32 c0;
    static u64 t0;
    u64 t = plat_ticks();
    u32 c = plat_idle_count();

    if (t0 != 0 && t - t0 >= (u64)plat_ticks_per_ms() * 500) {
        g_stats.cpu_load_x10 = 1000 - px_idle_x10(c - c0, t - t0);
        g_stats.uptime_ms = px_ms();
        c0 = c;
        t0 = t;
    } else if (t0 == 0) {
        c0 = c;
        t0 = t;
    }
}

u32 px_ms(void)
{
    u64 t = plat_ticks();

    if (g_t0 == 0)
        g_t0 = t;
#ifdef _IOP
    /* 36864 ticks per ms = 36 << 10: 32-bit division instead of __udivdi3
       (valid for 2^42 ticks, i.e. 33 hours) */
    return (u32)((t - g_t0) >> 10) / 36;
#else
    return (u32)((t - g_t0) / plat_ticks_per_ms());
#endif
}

void px_strlcpy(char *dst, const char *src, u32 size)
{
    u32 n = (u32)strlen(src);

    if (n >= size)
        n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

void px_set_error(const char *msg)
{
    px_strlcpy(g_stats.last_error, msg, sizeof(g_stats.last_error));
    g_stats.flags |= PX_SF_ERROR;
    px_log("error: %s\n", msg);
}
