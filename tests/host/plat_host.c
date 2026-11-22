/*
 * Host (Linux) implementation of iop/plat.h for the simulator:
 * pthreads + POSIX semaphores, a disk image file instead of ATA and a UDP
 * socket instead of SMAP.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "px.h"
#include "plat_host.h"

static pthread_mutex_t g_mutex;
static sem_t g_sems[32];
static int g_nsems;

int g_img_fd = -1;
u64 g_img_sectors;
int g_sock = -1;
u32 g_host_maxfree = 1500 * 1024;
u32 g_disk_mbps;  /* 0 = unlimited; otherwise simulated disk speed */
static struct sockaddr_in g_peer;
static pthread_mutex_t g_send_mutex = PTHREAD_MUTEX_INITIALIZER;

void plat_host_init(void)
{
    pthread_mutexattr_t a;

    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_mutex, &a);
}

int plat_lock(void)
{
    pthread_mutex_lock(&g_mutex);
    return 0;
}

void plat_unlock(int state)
{
    (void)state;
    pthread_mutex_unlock(&g_mutex);
}

int plat_sem_create(int initial, int max)
{
    (void)max;
    if (g_nsems >= 32)
        return -1;
    sem_init(&g_sems[g_nsems], 0, (unsigned)initial);
    return g_nsems++;
}

void plat_sem_wait(int sem)
{
    while (sem_wait(&g_sems[sem]) != 0) {}
}

void plat_sem_signal(int sem)
{
    sem_post(&g_sems[sem]);
}

u64 plat_ticks(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000u + (u64)ts.tv_nsec / 1000u;
}

u32 plat_ticks_per_ms(void)
{
    return 1000;
}

void plat_sleep_us(u32 usec)
{
    usleep(usec);
}

void *plat_alloc(u32 size)
{
    void *p = NULL;

    if (size > g_host_maxfree)
        return NULL;
    if (posix_memalign(&p, 64, size) != 0)
        return NULL;
    return p;
}

void plat_free(void *p)
{
    free(p);
}

u32 plat_max_free_mem(void) { return g_host_maxfree; }
u32 plat_total_free_mem(void) { return g_host_maxfree; }

static void disk_delay(u32 count)
{
    if (g_disk_mbps)
        usleep((useconds_t)((u64)count * 512 / g_disk_mbps)); /* bytes / (MB/s) = us */
}

int plat_bd_read(u64 lba, void *buf, u32 count)
{
    if (lba + count > g_img_sectors)
        return -1;
    if (pread(g_img_fd, buf, (size_t)count * 512, (off_t)(lba * 512)) != (ssize_t)count * 512)
        return -1;
    disk_delay(count);
    return 0;
}

int plat_bd_write(u64 lba, const void *buf, u32 count)
{
    if (lba + count > g_img_sectors) {
        fprintf(stderr, "plat_bd_write beyond the image: lba %llu count %u\n", (unsigned long long)lba, count);
        abort();
    }
    if (pwrite(g_img_fd, buf, (size_t)count * 512, (off_t)(lba * 512)) != (ssize_t)count * 512)
        return -1;
    disk_delay(count);
    return 0;
}

void plat_bd_flush(void)
{
    fdatasync(g_img_fd);
}

u64 plat_bd_sectors(void)
{
    return g_img_sectors;
}

int plat_send(const void *hdr, u32 hdr_len, const void *data, u32 data_len)
{
    unsigned char buf[2048];

    if (hdr_len + data_len > sizeof(buf))
        return -1;
    memcpy(buf, hdr, hdr_len);
    if (data != NULL)
        memcpy(buf + hdr_len, data, data_len);
    pthread_mutex_lock(&g_send_mutex);
    sendto(g_sock, buf, hdr_len + data_len, 0, (struct sockaddr *)&g_peer, sizeof(g_peer));
    pthread_mutex_unlock(&g_send_mutex);
    return 0;
}

void plat_set_peer(const void *peer)
{
    pthread_mutex_lock(&g_send_mutex);
    memcpy(&g_peer, peer, sizeof(g_peer) < PX_PEER_SIZE ? sizeof(g_peer) : PX_PEER_SIZE);
    pthread_mutex_unlock(&g_send_mutex);
}

u32 plat_idle_count(void) { return 0; }

void plat_periodic(void) {}
void plat_push_stop(void) {}
void plat_writer_boost(int on) { (void)on; }

u32 plat_bench_micro(u32 *vals, u32 n)
{
    (void)vals;
    (void)n;
    return 0;
}

int plat_bench_iomanx(u64 size, u32 chunk, u32 flags, u64 *wt, u32 *wi, u64 *rt, u32 *ri)
{
    (void)size; (void)chunk; (void)flags; (void)wt; (void)wi; (void)rt; (void)ri;
    return PX_ENOTSUP;
}
