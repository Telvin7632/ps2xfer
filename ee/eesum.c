/*
 * Verify helper: word-Fletcher sums on the EE (294 MHz, cached) instead of
 * the IOP (36 MHz). The IOP sends each chunk as the extra DMA of SIF command
 * PX_SIFCMD_CHUNK; the command handler (interrupt) queues it, a worker thread
 * sums it and answers with PX_SIFCMD_SUMS. Chunks arrive and are answered in
 * order, so a 1 MiB block can span two chunks with a running sum.
 */
#include <kernel.h>
#include <sifcmd.h>
#include <sifrpc.h>
#include <string.h>

#include "px_stats.h"
#include "eesum.h"

static u8 g_buf[2][PX_SUM_CHUNK] __attribute__((aligned(64)));
static SifCmdHandlerData_t g_cmds[1];

#define QN 8 /* the IOP has at most 2 chunks in flight */
static struct { u32 seq, buf, len, flags; } g_q[QN];
static volatile u32 g_qhead, g_qtail;
static int g_sema = -1;
static u8 g_stack[16 * 1024] __attribute__((aligned(16)));
static px_sumdone_t g_done __attribute__((aligned(64)));
static u32 g_arg[4] __attribute__((aligned(64)));

static void on_chunk(void *data, void *harg)
{
    const px_sumcmd_t *c = (const px_sumcmd_t *)data;
    u32 i = g_qhead % QN;

    (void)harg;
    g_q[i].seq   = c->seq;
    g_q[i].buf   = c->buf & 1;
    g_q[i].len   = c->len <= PX_SUM_CHUNK ? c->len : PX_SUM_CHUNK;
    g_q[i].flags = c->flags;
    g_qhead++;
    iSignalSema(g_sema);
}

static void worker(void *arg)
{
    u32 s1 = 0, s2 = 0;

    (void)arg;
    for (;;) {
        u32 i, n, len, flags;
        const u32 *w;

        WaitSema(g_sema);
        i     = g_qtail % QN;
        len   = g_q[i].len;
        flags = g_q[i].flags;
        w     = (const u32 *)g_buf[g_q[i].buf];
        if (flags & PX_SUM_FIRST)
            s1 = s2 = 0;
        if (len > 0) {
            /* the DMA wrote memory behind the cache */
            InvalidDCache((void *)w, (u8 *)w + len - 1);
            n = (len + 3) >> 2; /* the IOP zero-pads the tail */
            while (n >= 8) {
                s1 += w[0]; s2 += s1;
                s1 += w[1]; s2 += s1;
                s1 += w[2]; s2 += s1;
                s1 += w[3]; s2 += s1;
                s1 += w[4]; s2 += s1;
                s1 += w[5]; s2 += s1;
                s1 += w[6]; s2 += s1;
                s1 += w[7]; s2 += s1;
                w += 8;
                n -= 8;
            }
            while (n--) {
                s1 += *w++;
                s2 += s1;
            }
        }
        g_done.seq      = g_q[i].seq;
        g_done.flags    = flags;
        g_done.csum     = s2 ^ ((s1 << 16) | (s1 >> 16));
        g_done.reserved = 0;
        g_qtail++;
        while (SifSendCmd(PX_SIFCMD_SUMS, &g_done, sizeof(g_done), NULL, NULL, 0) == 0)
            ; /* SIF DMA queue full: retry */
    }
}

int eesum_start(SifRpcClientData_t *rpc, int main_priority)
{
    ee_sema_t sem;
    ee_thread_t t;
    int tid;
    static u8 reply[64] __attribute__((aligned(64)));

    memset(&sem, 0, sizeof(sem));
    sem.init_count = 0;
    sem.max_count  = QN;
    if ((g_sema = CreateSema(&sem)) < 0)
        return -1;

    memset(&t, 0, sizeof(t));
    t.func             = worker;
    t.stack            = g_stack;
    t.stack_size       = sizeof(g_stack);
    t.gp_reg           = &_gp;
    t.initial_priority = main_priority - 1; /* runs before the screen loop */
    if ((tid = CreateThread(&t)) < 0 || StartThread(tid, NULL) < 0)
        return -1;

    SifSetCmdBuffer(g_cmds, 1);
    SifAddCmdHandler(PX_SIFCMD_CHUNK, on_chunk, NULL);

    FlushCache(0); /* no dirty line may later overwrite DMA data */
    g_arg[0] = (u32)g_buf[0] & 0x1fffffff;
    g_arg[1] = (u32)g_buf[1] & 0x1fffffff;
    g_arg[2] = PX_SUM_CHUNK;
    g_arg[3] = 0;
    return SifCallRpc(rpc, PX_RPC_SET_SUM, 0, g_arg, sizeof(g_arg), reply, sizeof(reply), NULL, NULL) < 0 ? -1 : 0;
}
