/*
 * ps2xfer.elf - EE side: reads the configuration, resets the IOP, loads the
 * drivers and ps2xfer.irx, then only draws the status screen (SIF RPC poll
 * every 250 ms) and watches the pad. File data never passes through the EE.
 */
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <iopcontrol.h>
#include <sbv_patches.h>
#include <libpad.h>
#include <libpwroff.h>
#include <elf-loader.h>
#include <debug.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "px_stats.h"
#include "proto.h"
#include "ui.h"

#define VERSION "0.1.0"

#define IRX_DECL(name)                 \
    extern unsigned char name##_irx[]; \
    extern unsigned int size_##name##_irx

IRX_DECL(iomanX);
IRX_DECL(poweroff);
IRX_DECL(ps2dev9);
IRX_DECL(bdm);
IRX_DECL(bdmfs_fatfs);
IRX_DECL(ata_bd);
IRX_DECL(ps2xfer);

#define MOD_NO_RESIDENT_END 1

/* ---------------------------------------------------------------- config */

#define MAX_ARGS 24
static char g_args[MAX_ARGS][48];
static int g_nargs;
static char g_cfg_src[64] = "defaults";
static char g_exit_elf[128]; /* EE-only key: ELF to start on exit (e.g. ps2link) */
static int g_screen_x, g_screen_y; /* EE-only keys: picture offset for the TV */
static int g_gs_ok;                /* gsKit screen up (else the old text screen) */
static char g_msg[96];             /* footer message */
static int g_calib;                /* > 0: show the screen_x/y hint (in 250 ms ticks) */
static int g_fs_iomanx;

static void add_arg(const char *kv)
{
    int i;
    size_t klen = strcspn(kv, "=");

    if (kv[klen] != '=')
        return;
    if (strncmp(kv, "screen_x=", 9) == 0) {
        g_screen_x = atoi(kv + 9);
        return;
    }
    if (strncmp(kv, "screen_y=", 9) == 0) {
        g_screen_y = atoi(kv + 9);
        return;
    }
    if (strncmp(kv, "exit_elf=", 9) == 0) { /* not an IOP module argument */
        if (g_exit_elf[0] == 0) {
            strncpy(g_exit_elf, kv + 9, sizeof(g_exit_elf) - 1);
            g_exit_elf[sizeof(g_exit_elf) - 1] = 0;
        }
        return;
    }
    for (i = 0; i < g_nargs; i++) /* later sources do not override earlier ones */
        if (strncmp(g_args[i], kv, klen + 1) == 0)
            return;
    if (g_nargs < MAX_ARGS) {
        strncpy(g_args[g_nargs], kv, sizeof(g_args[0]) - 1);
        g_args[g_nargs][sizeof(g_args[0]) - 1] = 0;
        if (strcmp(g_args[g_nargs], "fs_iomanx=1") == 0)
            g_fs_iomanx = 1;
        g_nargs++;
    }
}

static int read_file(const char *path, char *buf, int max)
{
    FILE *f = fopen(path, "rb");
    int n;

    if (f == NULL)
        return -1;
    n = (int)fread(buf, 1, max - 1, f);
    fclose(f);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return n;
}

/* ps2xfer.ini: key=value lines, '#' comments */
static int load_ini(const char *path)
{
    char buf[2048], *line, *save = NULL;

    if (read_file(path, buf, sizeof(buf)) < 0)
        return -1;
    for (line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char kv[160];
        int i = 0, j = 0;
        while (line[i] == ' ' || line[i] == '\t')
            i++;
        if (line[i] == '#' || line[i] == ';' || line[i] == 0)
            continue;
        for (; line[i] && j < (int)sizeof(kv) - 1; i++)
            if (line[i] != ' ' && line[i] != '\t')
                kv[j++] = line[i];
        kv[j] = 0;
        add_arg(kv);
    }
    return 0;
}

/* IPCONFIG.DAT of wLaunchELF / uLaunchELF: "IP NETMASK GATEWAY" */
static int load_ipconfig(const char *path)
{
    char buf[128], ip[20], kv[32];
    int i = 0, j = 0;

    if (read_file(path, buf, sizeof(buf)) <= 0)
        return -1;
    while (buf[i] == ' ' || buf[i] == '\t')
        i++;
    while (buf[i] && buf[i] != ' ' && buf[i] != '\t' && buf[i] != '\r' && buf[i] != '\n' && j < 19)
        ip[j++] = buf[i++];
    ip[j] = 0;
    if (j < 7)
        return -1;
    snprintf(kv, sizeof(kv), "ip=%s", ip);
    add_arg(kv);
    return 0;
}

static void read_config(int argc, char *argv[])
{
    char path[256];

    if (argc > 0 && argv[0] != NULL) {
        char *slash;
        strncpy(path, argv[0], sizeof(path) - 16);
        path[sizeof(path) - 16] = 0;
        slash = strrchr(path, '/');
        if (slash == NULL)
            slash = strrchr(path, ':');
        if (slash != NULL) {
            strcpy(slash + 1, "ps2xfer.ini");
            if (load_ini(path) == 0) {
                size_t n = strlen(path); /* keep the end of a long path */
                const char *tail = n >= sizeof(g_cfg_src) ? path + n - (sizeof(g_cfg_src) - 1) : path;
                memcpy(g_cfg_src, tail, strlen(tail) + 1);
            }
        }
    }
    if (load_ini("mc0:/PS2XFER/ps2xfer.ini") == 0 && strcmp(g_cfg_src, "defaults") == 0)
        strcpy(g_cfg_src, "mc0:/PS2XFER/ps2xfer.ini");
    if (load_ipconfig("mc0:/SYS-CONF/IPCONFIG.DAT") == 0 && strcmp(g_cfg_src, "defaults") == 0)
        strcpy(g_cfg_src, "mc0:/SYS-CONF/IPCONFIG.DAT");
    else if (load_ipconfig("mc1:/SYS-CONF/IPCONFIG.DAT") == 0 && strcmp(g_cfg_src, "defaults") == 0)
        strcpy(g_cfg_src, "mc1:/SYS-CONF/IPCONFIG.DAT");
}

/* ---------------------------------------------------------------- IOP */

static void reset_iop(void)
{
    SifInitRpc(0);
    while (!SifIopReset("", 0)) {}
    while (!SifIopSync()) {}
    SifInitRpc(0);
    sbv_patch_enable_lmb();
    sbv_patch_disable_prefix_check();
}

static int load_driver(const char *name, void *irx, unsigned int size, int arglen, const char *args)
{
    int res = 0;
    int id = SifExecModuleBuffer(irx, size, arglen, args, &res);
    int ok = (id >= 0) && ((res & 3) != MOD_NO_RESIDENT_END);

    scr_printf("    %-12s %s\n", name, ok ? "OK" : "FAILED");
    return ok ? 0 : -1;
}

static void stop(const char *msg)
{
    scr_printf("\n  %s\n  You can turn the console off now.\n", msg);
    SleepThread();
}

/* ---------------------------------------------------------------- pad */

static char pad_buf[256] __attribute__((aligned(64)));

static void pad_init(void)
{
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:PADMAN", 0, NULL);
    padInit(0);
    padPortOpen(0, 0, pad_buf);
}

static u32 pad_held(void)
{
    struct padButtonStatus b;
    int s = padGetState(0, 0);

    if ((s != PAD_STATE_STABLE && s != PAD_STATE_FINDCTP1) || padRead(0, 0, &b) == 0)
        return 0;
    return 0xffff ^ b.btns;
}

/* ---------------------------------------------------------------- RPC */

static SifRpcClientData_t g_rpc;
static px_stats_t g_st __attribute__((aligned(64)));
/* written by the IOP via SIF DMA (PX_RPC_SET_PUSH); read only uncached */
static px_stats_t g_push __attribute__((aligned(64)));
static u32 g_push_arg[4] __attribute__((aligned(64)));

/* copies the pushed status into g_st; 0 if nothing was pushed yet */
static int read_push(void)
{
    static px_stats_t a, b;
    const volatile u8 *u = (const volatile u8 *)((u32)&g_push | 0x20000000); /* uncached */
    int tries;

    for (tries = 0; tries < 4; tries++) { /* two equal copies = not torn */
        unsigned int i;
        for (i = 0; i < sizeof(a); i++)
            ((u8 *)&a)[i] = u[i];
        for (i = 0; i < sizeof(b); i++)
            ((u8 *)&b)[i] = u[i];
        if (memcmp(&a, &b, sizeof(a)) == 0) {
            if (a.version == 0)
                return 0;
            memcpy(&g_st, &a, sizeof(g_st));
            return 1;
        }
    }
    return 0;
}

static int rpc(int fn)
{
    return SifCallRpc(&g_rpc, fn, 0, NULL, 0, &g_st, sizeof(g_st), NULL, NULL);
}

static void poweroff_cb(void *arg)
{
    (void)arg;
    /* the IOP closes an open transfer resumably in its poweroff handler */
    poweroffShutdown();
}

/* ---------------------------------------------------------------- screen */

static u64 u64of(unsigned int lo, unsigned int hi) { return ((u64)hi << 32) | lo; }

static void line(int y, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    while (n < 76 && n >= 0)
        buf[n++] = ' ';
    buf[n < 0 ? 0 : n] = 0;
    scr_setXY(0, y);
    scr_printf("%s", buf);
}

static const char *link_text(unsigned int m)
{
    switch (m & 0x0f) {
        case 8: return "100 Mbit full duplex";
        case 4: return "100 Mbit HALF duplex  !! slow: check cable/switch";
        case 2: return "10 Mbit full duplex  !! slow";
        case 1: return "10 Mbit half duplex  !! slow";
        default: return "no link (waiting...)";
    }
}

static void draw(float cur, float avg, int exit_hold)
{
    const px_stats_t *s = &g_st;
    u64 fsize = u64of(s->file_size_lo, s->file_size_hi);
    u64 wr = u64of(s->written_lo, s->written_hi);
    u64 disk = u64of(s->disk_sectors_lo, s->disk_sectors_hi) * 512;
    int y = 4;
    char udma[8], freemb[16];

    if (s->udma_mode == 0xff)
        strcpy(udma, "?");
    else
        snprintf(udma, sizeof(udma), "%u", s->udma_mode);
    if (s->fs_free_mb == 0xffffffff)
        strcpy(freemb, "?");
    else
        snprintf(freemb, sizeof(freemb), "%u", s->fs_free_mb);

    line(y++, "  IP %u.%u.%u.%u  UDP %u  MAC %02x:%02x:%02x:%02x:%02x:%02x   (config: %s)",
         s->ip >> 24, (s->ip >> 16) & 255, (s->ip >> 8) & 255, s->ip & 255, s->port, s->mac[0], s->mac[1],
         s->mac[2], s->mac[3], s->mac[4], s->mac[5], g_cfg_src);
    line(y++, "  Link: %s%s   SPEED rev 0x%x, RX DMA block %u", link_text(s->link_mode),
         (s->link_mode & 0x40) ? " +PAUSE" : "", s->speed_rev, s->dma_block);
    if (s->flags & PX_SF_DISK_OK)
        line(y++, "  Disk: %.40s, %u.%u GB, UDMA %s", s->disk_model, (unsigned)(disk / 1000000000),
             (unsigned)(disk / 100000000 % 10), udma);
    else
        line(y++, "  Disk: not found");
    if (s->flags & PX_SF_IOMANX)
        line(y++, "  exFAT: owned by bdmfs_fatfs (fs_iomanx=1 bench mode)");
    else if (s->flags & PX_SF_FS_OK)
        line(y++, "  exFAT: %u MiB, free %s MiB, cluster %u KiB", s->fs_total_mb, freemb,
             s->cluster_size / 1024);
    else
        line(y++, "  exFAT: not mounted");
    line(y++, "  IOP: free %u KiB, buffers %u x %u KiB, CPU %u.%u%%", s->iop_free / 1024, s->nbuf,
         s->bufsize / 1024, s->cpu_load_x10 / 10, s->cpu_load_x10 % 10);
    y++;
    if (s->flags & (PX_SF_XFER | PX_SF_VERIFY | PX_SF_READ)) {
        float pct = fsize ? (float)wr * 100.0f / (float)fsize : 0;
        float rate = avg > 0.05f ? avg : cur; /* ETA from the average: steady */
        int eta = rate > 0 ? (int)((float)(fsize - wr) / (rate * 1e6f)) : 0;
        line(y++, "  %s %.56s", (s->flags & PX_SF_XFER) ? "File:" : (s->flags & PX_SF_VERIFY) ? "Verify:" : "To PC:",
             s->file);
        line(y++, "  Progress: %5.1f%%   %llu / %llu MiB", pct, wr >> 20, fsize >> 20);
        line(y++, "  Speed: %5.2f MB/s (avg %5.2f)   ETA %d:%02d:%02d", cur, avg, eta / 3600, eta / 60 % 60,
             eta % 60);
    } else {
        line(y++, "  %s", (s->flags & PX_SF_SESSION) ? "Connected. Idle." : "Waiting for the PC client...");
        line(y++, " ");
        line(y++, " ");
    }
    line(y++, "  Packets %u, retransmitted %u, duplicates %u, dropped %u", s->rx_pkts, s->rx_retx, s->rx_dups,
         s->rx_drops);
    line(y++, "  RX errors %u (FIFO overruns %u), checksum errors %u, disk errors %u", s->rx_errors,
         s->rx_overruns, s->csum_errors, s->disk_errors);
    line(y++, "  %s%s", (s->flags & PX_SF_ERROR) ? "Last error: " : "", s->last_error);
    y++;
    line(y++, exit_hold ? "  Keep holding START to exit..." : "  Hold START for 2 s to exit.");
}

/* new gsKit screen, or the old libdebug text screen if gsKit failed */
static void render(float cur, float avg, int exit_hold)
{
    if (g_gs_ok) {
        ui_draw(&g_st, g_cfg_src, cur, avg, g_msg, g_calib > 0);
    } else {
        draw(cur, avg, exit_hold);
        line(22, "  %s", g_msg);
    }
}

int main(int argc, char *argv[])
{
    char argbuf[MAX_ARGS * 48];
    int arglen = 0, i, hold = 0;
    u64 prev_rx = 0;
    clock_t prev_t;
    float cur = 0, avg = 0;
    u64 xfer_start_bytes = 0;
    clock_t xfer_start_t = 0;
    u32 was_task = 0;          /* PX_SF_XFER / VERIFY / READ of the last poll */
    char task_file[64] = "";

    read_config(argc, argv);
    reset_iop();
    init_scr();
    scr_printf("\n  ps2xfer v" VERSION " - PC -> PS2 HDD (exFAT) over Ethernet\n\n");
    pad_init();

    for (i = 0; i < g_nargs; i++) {
        int n = (int)strlen(g_args[i]) + 1;
        memcpy(argbuf + arglen, g_args[i], n);
        arglen += n;
    }

    scr_printf("  Loading drivers:\n");
    if (load_driver("iomanX", iomanX_irx, size_iomanX_irx, 0, NULL) ||
        load_driver("poweroff", poweroff_irx, size_poweroff_irx, 0, NULL) ||
        load_driver("ps2dev9", ps2dev9_irx, size_ps2dev9_irx, 0, NULL) ||
        load_driver("bdm", bdm_irx, size_bdm_irx, 0, NULL) ||
        (g_fs_iomanx && load_driver("bdmfs_fatfs", bdmfs_fatfs_irx, size_bdmfs_fatfs_irx, 0, NULL)) ||
        load_driver("ata_bd", ata_bd_irx, size_ata_bd_irx, 0, NULL) ||
        load_driver("ps2xfer", ps2xfer_irx, size_ps2xfer_irx, arglen, argbuf))
        stop("Driver error. Is the network adapter connected?");

    poweroffInit();
    poweroffSetCallback(poweroff_cb, NULL);

    while (SifBindRpc(&g_rpc, PX_RPC_SID, 0) < 0 || g_rpc.server == NULL)
        nopdelay();

    /* ask the IOP to push its status (the RPC thread is starved during a
       transfer); flush first so no dirty cache line overwrites the DMA data */
    memset(&g_push, 0, sizeof(g_push));
    FlushCache(0);
    g_push_arg[0] = (u32)&g_push & 0x1fffffff;
    SifCallRpc(&g_rpc, PX_RPC_SET_PUSH, 0, g_push_arg, sizeof(g_push_arg), &g_st, sizeof(g_st), NULL, NULL);

    g_gs_ok = (ui_init(g_screen_x, g_screen_y) == 0);
    if (!g_gs_ok) {
        scr_clear();
        scr_setXY(0, 1);
        scr_printf("  ps2xfer v" VERSION " - PC -> PS2 HDD (exFAT) over Ethernet");
    }
    prev_t = clock();
    for (;;) {
        clock_t now;
        u64 rx;
        u32 btn;

        {
            /* the pushed status, unless it went stale (push stopped on
               EXIT, or not started): then ask by RPC */
            static u32 last_up, stale;
            if (read_push() && g_st.uptime_ms != last_up) {
                last_up = g_st.uptime_ms;
                stale = 0;
            } else if (++stale >= 2) {
                rpc(PX_RPC_GET_STATS);
            }
        }
        now = clock();
        rx = u64of(g_st.written_lo, g_st.written_hi);
        {
            /* a new write, verify or read (or the next file of one): new speed/ETA */
            u32 task = g_st.flags & (PX_SF_XFER | PX_SF_VERIFY | PX_SF_READ);
            if (task && (task != was_task || strncmp(task_file, g_st.file, sizeof(task_file)) != 0)) {
                xfer_start_bytes = rx;
                xfer_start_t = now;
                prev_rx = rx;
                cur = avg = 0;
                memcpy(task_file, g_st.file, sizeof(task_file) - 1);
            }
            was_task = task;
        }
        /* every poll (4x per second): instantaneous rate, smoothed with a
           ~1 s time constant (the written counter moves in 352 KiB steps) */
        if (now - prev_t >= CLOCKS_PER_SEC / 8) {
            float dt = (float)(now - prev_t) / CLOCKS_PER_SEC;
            if (was_task && rx >= prev_rx) {
                float inst = (float)(rx - prev_rx) / dt / 1e6f;
                float a = dt / (1.0f + dt);
                cur = (cur <= 0.0f) ? inst : cur + (inst - cur) * a;
                if (now > xfer_start_t)
                    avg = (float)(rx - xfer_start_bytes) / ((float)(now - xfer_start_t) / CLOCKS_PER_SEC) / 1e6f;
            }
            prev_rx = rx;
            prev_t = now;
        }

        btn = pad_held();
        hold = (btn & PAD_START) ? hold + 1 : 0;
        {
            /* D-pad moves the picture (TV calibration); edge-triggered */
            static u32 prev_btn;
            u32 pressed = btn & ~prev_btn;
            int dx = (pressed & PAD_RIGHT) ? 4 : (pressed & PAD_LEFT) ? -4 : 0;
            int dy = (pressed & PAD_DOWN) ? 2 : (pressed & PAD_UP) ? -2 : 0;
            prev_btn = btn;
            if (g_gs_ok && (dx || dy)) {
                ui_move(ui_off_x() + dx, ui_off_y() + dy);
                g_calib = 40;
            }
        }
        if (g_calib > 0)
            g_calib--;
        if (hold > 0 && hold < 8)
            snprintf(g_msg, sizeof(g_msg), "Keep holding START to exit...");
        else if (hold == 0 && strncmp(g_msg, "Keep", 4) == 0)
            g_msg[0] = 0;
        render(cur, avg, hold > 0);
        if (hold >= 8) { /* 8 x 250 ms */
            snprintf(g_msg, sizeof(g_msg), "Exiting: closing files...");
            render(cur, avg, 0);
            rpc(PX_RPC_EXIT);
        }
        if (g_st.flags & PX_SF_EXIT) {
            snprintf(g_msg, sizeof(g_msg), "%s", g_exit_elf[0] ? "Bye. Starting the exit_elf program."
                                                               : "Bye. Returning to the browser.");
            render(cur, avg, 0);
            {
                clock_t t = clock();
                while (clock() - t < CLOCKS_PER_SEC) {}
            }
            if (g_exit_elf[0]) { /* e.g. mc0:/PS2LINK/PS2LINK.ELF */
                /* elf-loader checks the file through the current IOP: load the
                   memory card driver (the IOP module freed its write buffers on
                   EXIT to make room). The loader then resets the IOP itself. */
                int r1 = 0, r2 = 0, rl;

                if (strncmp(g_exit_elf, "mc", 2) == 0) {
                    r1 = SifLoadModule("rom0:MCMAN", 0, NULL);
                    r2 = SifLoadModule("rom0:MCSERV", 0, NULL);
                }
                sbv_patch_disable_prefix_check();
                rl = LoadELFFromFile(g_exit_elf, 0, NULL);
                /* only reached on failure */
                snprintf(g_msg, sizeof(g_msg), "exit_elf %.40s failed: %d (MCMAN %d, MCSERV %d)", g_exit_elf, rl,
                         r1, r2);
                render(cur, avg, 0);
                {
                    clock_t t = clock();
                    while (clock() - t < 5 * CLOCKS_PER_SEC) {}
                }
            }
            Exit(0);
        }
        {
            clock_t t = clock();
            while (clock() - t < CLOCKS_PER_SEC / 4) {}
        }
    }
    return 0;
}
