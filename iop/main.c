/*
 * ps2xfer.irx - receives files over Ethernet and writes them to the exFAT
 * partition of the internal HDD. See docs/DESIGN.md.
 *
 * Module arguments: key=value (see px_cfg_set), e.g. ip=192.168.0.10 bufs=3.
 *
 * Threads (IOP: lower number = higher priority):
 *   SMAP RX      rx_prio (10)  frames -> fast path / ARP / ICMP / control mailbox
 *   ack          12            periodic and on-demand ACKs
 *   writer       20            full buffers -> disk
 *   control      30            commands, FatFs, benches, start-up
 *   rpc          40            SIF RPC: status for the EE (priority 14 hung the
 *                              IOP on a real PS2, 2026-11-22)
 *   idle         126           CPU load meter
 */
#include "irx_imports.h"
#include <speedregs.h>
#include "px.h"
#include "smap.h"

IRX_ID("ps2xfer", 0, 1);

void net_init(u32 ip, u16 port);
void net_start(void);
int plat_find_disk(void);
void plat_identify(void);
void plat_idle_thread(void *arg);
void rpc_thread(void *arg);
extern int g_writer_tid;

static iop_sys_clock_t g_ack_clk;

static unsigned int ack_alarm(void *arg)
{
    (void)arg;
    /* link state for the screen and HELLO (plain reads, safe here) */
    if (SmapDriverData.SmapIsInitialized && SmapDriverData.LinkStatus) {
        g_stats.link_mode = SmapDriverData.LinkMode;
        g_stats.flags |= PX_SF_NET_UP;
    } else {
        g_stats.link_mode = 0;
        g_stats.flags &= ~PX_SF_NET_UP;
    }
    iSignalSema(g_ack_sem);
    return g_ack_clk.lo;
}

static int start_thread(void (*fn)(void *), int prio, int stack)
{
    iop_thread_t t;
    int tid;

    t.attr      = TH_C;
    t.option    = 0;
    t.thread    = fn;
    t.stacksize = stack;
    t.priority  = prio;
    tid = CreateThread(&t);
    if (tid >= 0)
        StartThread(tid, NULL);
    return tid;
}

static void poweroff_cb(void *arg)
{
    (void)arg;
    /* runs in poweroff.irx's thread when the EE calls poweroffShutdown():
       keep an interrupted transfer resumable and flush the disk cache */
    ctrl_shutdown_sync();
}

/* start-up work that may sleep (disk spin-up, PHY auto-negotiation) */
static void init_thread(void *arg)
{
    USE_SPD_REGS;
    int r;

    (void)arg;
    px_idle_calibrate(200);
    g_stats.speed_rev = SPD_REG16(SPD_R_REV_1);

    if ((r = smap_init(g_cfg.rx_prio, g_cfg.pause)) < 0) {
        px_log("smap_init failed: %d\n", r);
        px_set_error("network adapter not found");
    } else {
        net_init(g_cfg.ip, (u16)g_cfg.port);
        net_start();
        g_stats.dma_block = (g_cfg.dma_block && g_cfg.dma_block < g_dma_max_block_size) ? g_cfg.dma_block
                                                                                        : g_dma_max_block_size;
    }

    if (plat_find_disk() == 0) {
        g_stats.flags |= PX_SF_DISK_OK;
        {
            u64 n = plat_bd_sectors();
            g_stats.disk_sectors_lo = (u32)n;
            g_stats.disk_sectors_hi = (u32)(n >> 32);
        }
        plat_identify();
        if (!g_cfg.fs_iomanx) {
            fsops_mount();
            fsops_update_free();
        }
    } else {
        px_set_error("internal HDD (ata0) not found");
    }

    /* the write buffers take what is left */
    rxeng_alloc();
    g_stats.iop_free    = QueryTotalFreeMemSize();
    g_stats.iop_maxfree = QueryMaxFreeMemSize();
    px_log("IOP free after setup: %u (max block %u)\n", (unsigned)g_stats.iop_free,
           (unsigned)g_stats.iop_maxfree);

    USec2SysClock(g_cfg.ack_ms * 1000, &g_ack_clk);
    SetAlarm(&g_ack_clk, ack_alarm, NULL);

    /* this thread becomes the control thread */
    ctrl_thread(NULL);
}

int _start(int argc, char *argv[])
{
    int i;

    px_cfg_defaults(&g_cfg);
    for (i = 1; i < argc; i++)
        px_cfg_set(&g_cfg, argv[i], 0);

    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.version    = PX_STATS_VERSION;
    g_stats.ip         = g_cfg.ip;
    g_stats.port       = g_cfg.port;
    g_stats.udma_mode  = 0xff;
    g_stats.fs_total_mb = 0xffffffff;
    g_stats.fs_free_mb = 0xffffffff;
    if (g_cfg.fs_iomanx)
        g_stats.flags |= PX_SF_IOMANX;

    px_log_init();
    px_log("ps2xfer " PX_VERSION_STR " starting\n");
    if (px_disk_init() != 0 || ctrl_init() != 0)
        return MODULE_NO_RESIDENT_END;
    g_writer_sem = plat_sem_create(0, 64);
    g_ack_sem    = plat_sem_create(0, 64);
    if (g_writer_sem < 0 || g_ack_sem < 0)
        return MODULE_NO_RESIDENT_END;

    AddPowerOffHandler(poweroff_cb, NULL);

    if (start_thread(plat_idle_thread, 126, 0x200) < 0 ||
        start_thread(ack_thread, 12, 0x1000) < 0 ||
        (g_writer_tid = start_thread(writer_thread, 20, 0x1000)) < 0 ||
        start_thread(rpc_thread, 40, 0x1000) < 0 ||
        start_thread(init_thread, 30, 0x2000) < 0)
        return MODULE_NO_RESIDENT_END;

    return MODULE_RESIDENT_END;
}
