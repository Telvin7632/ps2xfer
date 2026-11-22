/*
 * ps2xfer-sim: the portable core of ps2xfer.irx (protocol, receive engine,
 * writer, FatFs, resume state) running on Linux with a UDP socket and a disk
 * image. Used to test pc/ps2xfer.py end to end without a PS2.
 *
 *   ps2xfer-sim --image IMG [--mkfs MIB] [--port N] [--drop PCT]
 *               [--disk-mbps N] [--maxfree BYTES] [key=value ...]
 *
 * --mkfs creates IMG (sparse) as MBR + exFAT, like ps2-exfat-format.
 * --drop drops that percentage of incoming DATA packets (loss injection),
 * --drop-tx that percentage of outgoing READ_DATA packets.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "px.h"
#include "ff.h"
#include "plat_host.h"

static double g_drop_pct;
static volatile int g_ack_timer_run = 1;

static void *thr_writer(void *a) { writer_thread(a); return NULL; }
static void *thr_ack(void *a) { ack_thread(a); return NULL; }
static void *thr_ctrl(void *a) { ctrl_thread(a); return NULL; }

static void *thr_ack_timer(void *a)
{
    (void)a;
    while (g_ack_timer_run) {
        usleep(g_cfg.ack_ms * 1000);
        plat_sem_signal(g_ack_sem);
    }
    return NULL;
}

static void payload_copy(void *ctx, void *dst, u32 len)
{
    memcpy(dst, ctx, len);
}

static int do_mkfs(const char *path, unsigned mib)
{
    static BYTE work[65536];
    FATFS fs;
    MKFS_PARM opt;
    FRESULT fr;

    g_img_fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (g_img_fd < 0 || ftruncate(g_img_fd, (off_t)mib << 20) != 0) {
        perror(path);
        return -1;
    }
    g_img_sectors = (u64)mib << 11;
    memset(&opt, 0, sizeof(opt));
    opt.fmt = FM_EXFAT;
    fr = f_mkfs("", &opt, work, sizeof(work));
    if (fr != FR_OK) {
        fprintf(stderr, "f_mkfs: %d\n", (int)fr);
        return -1;
    }
    (void)fs;
    return 0;
}

int main(int argc, char **argv)
{
    const char *image = NULL;
    unsigned mkfs_mib = 0;
    int i;
    pthread_t t;
    struct sockaddr_in addr;
    static u8 rxbuf[2048 + 4] __attribute__((aligned(4)));
    u8 *pkt = rxbuf + 2; /* DATA header at 2 mod 4, like in the SMAP buffer */

    plat_host_init();
    px_cfg_defaults(&g_cfg);
    g_cfg.ip = 0x7f000001;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--image") == 0 && i + 1 < argc)
            image = argv[++i];
        else if (strcmp(argv[i], "--mkfs") == 0 && i + 1 < argc)
            mkfs_mib = (unsigned)atoi(argv[++i]);
        else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            g_cfg.port = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--drop") == 0 && i + 1 < argc)
            g_drop_pct = atof(argv[++i]);
        else if (strcmp(argv[i], "--drop-tx") == 0 && i + 1 < argc)
            g_drop_tx_pct = atof(argv[++i]);
        else if (strcmp(argv[i], "--disk-mbps") == 0 && i + 1 < argc)
            g_disk_mbps = (u32)atoi(argv[++i]);
        else if (strcmp(argv[i], "--maxfree") == 0 && i + 1 < argc)
            g_host_maxfree = (u32)atoi(argv[++i]);
        else if (px_cfg_set(&g_cfg, argv[i], 0) != 0) {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (image == NULL) {
        fprintf(stderr, "usage: ps2xfer-sim --image IMG [--mkfs MIB] [--port N] [--drop PCT] "
                        "[--disk-mbps N] [--maxfree BYTES] [key=value ...]\n");
        return 2;
    }

    px_log_init();
    if (px_disk_init() != 0 || ctrl_init() != 0)
        return 1;
    g_writer_sem = plat_sem_create(0, 1000);
    g_ack_sem    = plat_sem_create(0, 1000);

    if (mkfs_mib) {
        if (do_mkfs(image, mkfs_mib) != 0)
            return 1;
    } else {
        struct stat st;
        g_img_fd = open(image, O_RDWR);
        if (g_img_fd < 0 || fstat(g_img_fd, &st) != 0) {
            perror(image);
            return 1;
        }
        g_img_sectors = (u64)st.st_size / 512;
    }
    g_stats.version = PX_STATS_VERSION;
    g_stats.ip      = g_cfg.ip;
    g_stats.port    = g_cfg.port;
    g_stats.link_mode = 8;
    g_stats.udma_mode = 0xff;
    g_stats.flags  |= PX_SF_NET_UP | PX_SF_DISK_OK;
    g_stats.disk_sectors_lo = (u32)g_img_sectors;
    g_stats.disk_sectors_hi = (u32)(g_img_sectors >> 32);
    strcpy(g_stats.disk_model, "host image");
    memcpy(g_stats.mac, "\x02\x00\x00\x00\x00\x01", 6);
    g_stats.fs_free_mb = 0xffffffff;

    if (rxeng_alloc() != 0)
        return 1;
    fsops_mount();
    fsops_update_free();

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    {
        int sz = 4 << 20;
        setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &sz, sizeof(sz));
    }
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u16)g_cfg.port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(g_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        perror("bind");
        return 1;
    }

    pthread_create(&t, NULL, thr_writer, NULL);
    pthread_create(&t, NULL, thr_ack, NULL);
    pthread_create(&t, NULL, thr_ack_timer, NULL);
    pthread_create(&t, NULL, thr_ctrl, NULL);
    fprintf(stderr, "ps2xfer-sim: listening on UDP %u, image %s (%llu MiB)\n", (unsigned)g_cfg.port,
            image, (unsigned long long)(g_img_sectors >> 11));

    {
        struct timeval tv = {0, 200000};
        setsockopt(g_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    }
    srand(12345);
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        ssize_t n = recvfrom(g_sock, pkt, 2048, 0, (struct sockaddr *)&from, &fl);
        u8 peer[PX_PEER_SIZE];

        if (g_stats.flags & PX_SF_EXIT) {
            usleep(200000);
            fprintf(stderr, "ps2xfer-sim: EXIT\n");
            break;
        }
        if (n <= 0)
            continue;
        if (pkt[0] == PX_T_DATA && n >= PX_DATA_HDR) {
            if (g_drop_pct > 0 && (double)rand() / RAND_MAX * 100.0 < g_drop_pct)
                continue;
            if ((u32)n < PX_DATA_HDR + ((rd16(pkt + 20) + 3u) & ~3u)) {
                g_stats.rx_drops++;
                continue;
            }
            if (rxeng_data(pkt, payload_copy, pkt + PX_DATA_HDR))
                rxeng_maybe_ack(1);
        } else {
            memset(peer, 0, sizeof(peer));
            memcpy(peer, &from, sizeof(peer));
            ctrl_enqueue(pkt, (u32)n, peer);
        }
    }
    g_ack_timer_run = 0;
    close(g_img_fd);
    return 0;
}
