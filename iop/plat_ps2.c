/*
 * IOP implementation of plat.h (plat_send/plat_set_peer are in net.c).
 */
#include "irx_imports.h"
#include <speedregs.h>
#include "px.h"
#include "smap.h"

static struct block_device *g_bd;
static volatile u32 g_idle_count;

int plat_lock(void)
{
    int st;

    CpuSuspendIntr(&st);
    return st;
}

void plat_unlock(int state)
{
    CpuResumeIntr(state);
}

int plat_sem_create(int initial, int max)
{
    iop_sema_t s;

    s.attr    = SA_THPRI;
    s.option  = 0;
    s.initial = initial;
    s.max     = max;
    return CreateSema(&s);
}

void plat_sem_wait(int sem) { WaitSema(sem); }
void plat_sem_signal(int sem) { SignalSema(sem); }

u64 plat_ticks(void)
{
    iop_sys_clock_t c;

    GetSystemTime(&c);
    return ((u64)c.hi << 32) | c.lo;
}

u32 plat_ticks_per_ms(void)
{
    return 36864; /* IOP bus clock: 36.864 MHz */
}

void plat_sleep_us(u32 usec)
{
    DelayThread((int)usec);
}

void *plat_alloc(u32 size)
{
    return AllocSysMemory(ALLOC_FIRST, (int)size, NULL); /* 256-byte aligned blocks */
}

void plat_free(void *p)
{
    FreeSysMemory(p);
}

u32 plat_max_free_mem(void) { return QueryMaxFreeMemSize(); }
u32 plat_total_free_mem(void) { return QueryTotalFreeMemSize(); }

/* ---------------------------------------------------------------- disk */

#define MAX_BD 20
#define BD_MAX_COUNT 0x8000

/* raw whole-disk device of the internal HDD: "ata", unit 0, partition 0
   (same selection as ps2-exfat-format; never USB, never ata0pN) */
int plat_find_disk(void)
{
    struct block_device *list[MAX_BD];
    int tries, i;

    for (tries = 0; tries < 20; tries++) {
        memset(list, 0, sizeof(list));
        bdm_get_bd(list, MAX_BD);
        for (i = 0; i < MAX_BD; i++) {
            struct block_device *bd = list[i];
            if (bd != NULL && bd->name != NULL && strcmp(bd->name, "ata") == 0 &&
                bd->devNr == 0 && bd->parNr == 0 && bd->sectorCount > 0 && bd->sectorSize == 512) {
                g_bd = bd;
                return 0;
            }
        }
        DelayThread(500 * 1000);
    }
    return -1;
}

int plat_bd_read(u64 lba, void *buf, u32 count)
{
    u8 *p = (u8 *)buf;

    if (g_bd == NULL)
        return -1;
    while (count) {
        u16 n = count > BD_MAX_COUNT ? BD_MAX_COUNT : (u16)count;
        if (g_bd->read(g_bd, lba, p, n) != n)
            return -1;
        lba += n;
        p += (u32)n * 512;
        count -= n;
    }
    return 0;
}

int plat_bd_write(u64 lba, const void *buf, u32 count)
{
    const u8 *p = (const u8 *)buf;

    if (g_bd == NULL)
        return -1;
    while (count) {
        u16 n = count > BD_MAX_COUNT ? BD_MAX_COUNT : (u16)count;
        if (g_bd->write(g_bd, lba, p, n) != n)
            return -1;
        lba += n;
        p += (u32)n * 512;
        count -= n;
    }
    return 0;
}

void plat_bd_flush(void)
{
    if (g_bd != NULL && g_bd->flush != NULL)
        g_bd->flush(g_bd);
}

u64 plat_bd_sectors(void)
{
    return g_bd != NULL ? g_bd->sectorCount : 0;
}

/* ATA IDENTIFY DEVICE: model string and the selected UDMA mode (word 88) */
void plat_identify(void)
{
    static u16 id[256] __attribute__((aligned(64)));
    int i, r;

    if (g_bd == NULL)
        return;
    px_ata_lock();
    r = sceAtaExecCmd(id, 1, 0, 0, 0, 0, 0, (u16)((g_bd->devNr << 4) & 0xffff), 0xEC);
    if (r == 0)
        r = sceAtaWaitResult();
    px_ata_unlock();
    if (r != 0) {
        px_log("IDENTIFY failed: %d\n", r);
        return;
    }
    for (i = 0; i < 20; i++) { /* words 27..46, two chars per word, high byte first */
        g_stats.disk_model[i * 2]     = (char)(id[27 + i] >> 8);
        g_stats.disk_model[i * 2 + 1] = (char)id[27 + i];
    }
    g_stats.disk_model[40] = 0;
    for (i = 39; i >= 0 && g_stats.disk_model[i] == ' '; i--)
        g_stats.disk_model[i] = 0;
    g_stats.udma_mode = 0xff;
    if (id[53] & 4) { /* word 88 valid */
        for (i = 6; i >= 0; i--)
            if (id[88] & (0x100 << i)) {
                g_stats.udma_mode = (u32)i;
                break;
            }
    }
    px_log("disk: %s, UDMA %d\n", g_stats.disk_model, (int)g_stats.udma_mode);
}

/* ---------------------------------------------------------------- idle / bench */

void net_announce(void);

extern u32 g_push_addr; /* rpc.c */

/* status push to the EE by SIF DMA (see PX_RPC_SET_PUSH) */
static int g_push_trid = -1;

static void push_stats(u32 now)
{
    static px_stats_t buf __attribute__((aligned(64)));
    static u32 last;
    int trid = g_push_trid;
    SifDmaTransfer_t t;
    int st;

    if (g_push_addr == 0 || now - last < 250)
        return;
    if (trid >= 0 && sceSifDmaStat(trid) >= 0)
        return; /* previous push still in flight */
    last = now;
    g_stats.uptime_ms = now;
    memcpy(&buf, &g_stats, sizeof(buf));
    t.src  = &buf;
    t.dest = (void *)g_push_addr;
    t.size = sizeof(buf);
    t.attr = 0;
    CpuSuspendIntr(&st);
    g_push_trid = sceSifSetDma(&t, 1);
    CpuResumeIntr(st);
}

/* called from the control thread, which never preempts the ack thread (the
   pusher), so no new push starts after g_push_addr is cleared */
void plat_push_stop(void)
{
    int i;

    g_push_addr = 0;
    for (i = 0; i < 100 && g_push_trid >= 0 && sceSifDmaStat(g_push_trid) >= 0; i++)
        DelayThread(1000);
}

void plat_periodic(void)
{
    static u32 last;
    u32 now = px_ms();

    push_stats(now);

    if (!(g_stats.flags & PX_SF_SESSION) && (g_stats.flags & PX_SF_NET_UP) && now - last >= 2000) {
        last = now;
        net_announce();
    }
}

#define WRITER_PRIO 20 /* main.c */
int g_writer_tid = -1;  /* main.c */

void plat_writer_boost(int on)
{
    if (GetThreadId() != g_writer_tid) /* only the writer thread, never ctrl/bench */
        return;
    ChangeThreadPriority(TH_SELF, on ? (int)g_cfg.rx_prio - 1 : WRITER_PRIO);
}

u32 plat_idle_count(void)
{
    return g_idle_count;
}

void plat_idle_thread(void *arg)
{
    (void)arg;
    for (;;)
        g_idle_count++;
}

u32 plat_bench_micro(u32 *vals, u32 n)
{
    USE_SPD_REGS;
    volatile u16 sink;
    u64 t0;
    u32 i, k = 0;

    if (n < 4)
        return 0;
    /* SPEED register read (SSBUS) x1000 */
    t0 = plat_ticks();
    for (i = 0; i < 1000; i++)
        sink = SPD_REG16(SPD_R_REV_1);
    vals[k++] = (u32)(plat_ticks() - t0);
    /* GetSystemTime x1000 (the cost of our own timestamps) */
    t0 = plat_ticks();
    for (i = 0; i < 1000; i++)
        (void)plat_ticks();
    vals[k++] = (u32)(plat_ticks() - t0);
    vals[k++] = SPD_REG16(SPD_R_REV_1);
    vals[k++] = g_dma_max_block_size;
    (void)sink;
    return k;
}

/* Does what OPL does with the games: open "mass0:DVD", list it, open each
   .iso and read sector 16 (ISO9660 "CD001"). Only in fs_iomanx mode, where
   bdmfs_fatfs (as in OPL) owns the volume. */
int plat_fscheck(char *out, u32 max)
{
    static u8 sec[2048 + 64] __attribute__((aligned(64)));
    iox_dirent_t de;
    int dfd, fd, n = 0, r;
    u32 len = 0;
    char path[300];

    if (!g_cfg.fs_iomanx)
        return PX_ENOTSUP;
    out[0] = 0;
    dfd = dopen("mass0:DVD");
    len += sprintf(out + len, "dopen mass0:DVD = %d\n", dfd);
    if (dfd < 0)
        return PX_OK;
    while ((r = dread(dfd, &de)) > 0 && len + 200 < max) {
        if (!FIO_S_ISREG(de.stat.mode))
            continue;
        sprintf(path, "mass0:DVD/%s", de.name);
        fd = open(path, O_RDONLY);
        if (fd < 0) {
            len += sprintf(out + len, "%.60s: open %d\n", de.name, fd);
            continue;
        }
        /* sector 16 twice: into a 64-byte aligned buffer and into one that
           starts 1 byte later (OPL's isofs buffer has no alignment) */
        {
            int k, res[2];
            for (k = 0; k < 2; k++) {
                u8 *b = sec + k;
                memset(sec, 0, sizeof(sec));
                r = lseek(fd, 16 * 2048, SEEK_SET);
                n = (r >= 0) ? read(fd, b, 2048) : r;
                res[k] = (n == 2048 && b[0] == 1 && memcmp(b + 1, "CD001", 5) == 0);
            }
            len += sprintf(out + len, "%.48s: aligned %s, buffer+1 %s\n", de.name, res[0] ? "CD001 OK" : "BAD",
                           res[1] ? "CD001 OK" : "BAD");
        }
        close(fd);
    }
    dclose(dfd);
    return PX_OK;
}

int plat_bench_iomanx(u64 size, u32 chunk, u32 flags, u64 *wt, u32 *wi, u64 *rt, u32 *ri)
{
    const char *path = "mass0:/ps2xfer_bench.bin";
    u64 done, t0;
    u32 c0;
    int fd, r = PX_OK;

    if (!g_cfg.fs_iomanx)
        return PX_ENOTSUP; /* bdmfs_fatfs is only loaded in fs_iomanx mode */
    if (chunk == 0 || chunk > g_bufmem_size)
        chunk = g_bufmem_size;
    chunk &= ~511u;

    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0)
        return PX_EIO;
    c0 = plat_idle_count();
    t0 = plat_ticks();
    for (done = 0; done < size; done += chunk) {
        u32 n = (size - done < chunk) ? (u32)(size - done) : chunk;
        if (write(fd, g_bufmem, (int)n) != (int)n) {
            r = PX_EIO;
            break;
        }
    }
    close(fd);
    *wt = plat_ticks() - t0;
    *wi = px_idle_x10(plat_idle_count() - c0, *wt);

    if (r == PX_OK && (flags & 1) && (fd = open(path, O_RDONLY)) >= 0) {
        c0 = plat_idle_count();
        t0 = plat_ticks();
        for (done = 0; done < size; done += chunk) {
            u32 n = (size - done < chunk) ? (u32)(size - done) : chunk;
            if (read(fd, g_bufmem, (int)n) != (int)n) {
                r = PX_EIO;
                break;
            }
        }
        close(fd);
        *rt = plat_ticks() - t0;
        *ri = px_idle_x10(plat_idle_count() - c0, *rt);
    }
    remove(path);
    return r;
}
