/*
 * Verify helper on the EE (see common/px_stats.h, PX_RPC_SET_SUM): the IOP
 * sends disk chunks by SIF command + extra DMA and gets the sums back as a
 * SIF command. PS2 only; fsops.c falls back to IOP sums when not ready.
 */
#include "irx_imports.h"
#include "px.h"

static u32 g_ee_buf[2];        /* EE physical addresses, 0 = no helper */
static SifCmdHandlerData_t g_cmds[1];
static px_sumcmd_t g_cmd[2] __attribute__((aligned(64)));

#define RN 4 /* at most 2 chunks in flight */
static volatile struct { u32 seq, csum, flags, valid; } g_res[RN];

static void on_sums(void *data, void *harg)
{
    const px_sumdone_t *d = (const px_sumdone_t *)data;
    u32 i = d->seq % RN;

    (void)harg;
    g_res[i].seq   = d->seq;
    g_res[i].csum  = d->csum;
    g_res[i].flags = d->flags;
    g_res[i].valid = 1;
}

/* RPC thread: the EE registered its two chunk buffers */
void eesum_register(const u32 *arg)
{
    if (arg[2] != PX_SUM_CHUNK || arg[0] == 0 || arg[1] == 0) {
        px_log("EE verify helper: bad registration\n");
        return;
    }
    sceSifSetCmdBuffer(g_cmds, 1);
    sceSifAddCmdHandler(PX_SIFCMD_SUMS, on_sums, NULL);
    g_ee_buf[0] = arg[0];
    g_ee_buf[1] = arg[1];
    px_log("EE verify helper ready\n");
}

int plat_ee_sum_ready(void)
{
    return g_cfg.ee_sum && g_ee_buf[0] != 0;
}

void plat_ee_sum_disable(void)
{
    g_ee_buf[0] = g_ee_buf[1] = 0;
}

u32 plat_ee_sum_send(u32 seq, int buf, void *src, u32 len, u32 flags)
{
    u32 dlen = (len + 15) & ~15u;
    px_sumcmd_t *c = &g_cmd[buf & 1];

    if (g_ee_buf[0] == 0 || len == 0 || len > PX_SUM_CHUNK)
        return 0;
    if (dlen > len)
        memset((u8 *)src + len, 0, dlen - len);
    g_res[seq % RN].valid = 0;
    c->seq   = seq;
    c->buf   = (u32)buf & 1;
    c->len   = len;
    c->flags = flags;
    return sceSifSendCmd(PX_SIFCMD_CHUNK, c, sizeof(*c), src, (void *)g_ee_buf[buf & 1], (int)dlen);
}

int plat_ee_dma_done(u32 trid)
{
    return sceSifDmaStat((int)trid) < 0;
}

int plat_ee_sum_result(u32 seq, u32 *csum, u32 *flags)
{
    u32 i = seq % RN;

    if (!g_res[i].valid || g_res[i].seq != seq)
        return 0;
    *csum  = g_res[i].csum;
    *flags = g_res[i].flags;
    return 1;
}
