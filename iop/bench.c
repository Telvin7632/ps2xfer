/*
 * Phase 1 benchmarks that run inside the module (no network involved):
 *   MICRO       cost of the basic operations (Fletcher, memcpy, SPEED register
 *               reads, timer reads)
 *   DISK_RAW    RAM -> preallocated bench file through bounds-checked raw
 *               writes (bd->write), chunk = sectors per ATA command
 *   DISK_FATFS  the same through FatFs f_write, chunk = bytes per call
 *   DISK_IOMANX the same through bdmfs_fatfs + iomanX (fs_iomanx=1 boot only)
 * All disk benches write only inside /.ps2xfer/bench.bin, which is created
 * with f_expand() and deleted afterwards.
 *
 * Request: u32 kind, u32 size_mb, u32 chunk, u32 flags (bit0: read back too)
 * Response: u32 kind, u32 ticks_per_ms, u64 bytes, u64 write_ticks,
 *           u32 write_idle_x10, u32 0, u64 read_ticks, u32 read_idle_x10,
 *           u32 nvals, u32 vals[16]
 */
#ifdef _IOP
#include "irx_imports.h"
#else
#include <string.h>
#endif
#include "px.h"
#include "ff.h"

#define BENCH_RESP_LEN 112

static FIL g_bfil;

static void fill_pattern(u8 *p, u32 n)
{
    u32 i;

    for (i = 0; i + 4 <= n; i += 4)
        wr32(p + i, i * 2654435761u);
}

static int bench_raw(u64 size, u32 chunk, u32 flags, u64 *wt, u32 *wi, u64 *rt, u32 *ri)
{
    px_extent_t e;
    u64 done, t0;
    u32 c0, maxchunk = g_bufmem_size / 512;
    int r;

    if (chunk == 0 || chunk > maxchunk)
        chunk = maxchunk;
    if ((r = fsops_bench_create(&g_bfil, size, &e)) != PX_OK)
        return r;
    fill_pattern(g_bufmem, g_bufmem_size);

    c0 = plat_idle_count();
    t0 = plat_ticks();
    for (done = 0; done < size / 512 && r == PX_OK; done += chunk) {
        u32 n = (size / 512 - done < chunk) ? (u32)(size / 512 - done) : chunk;
        r = px_extent_write(&e, done, g_bufmem, n, n);
    }
    px_ata_lock();
    plat_bd_flush();
    px_ata_unlock();
    *wt = plat_ticks() - t0;
    *wi = px_idle_x10(plat_idle_count() - c0, *wt);

    if (r == PX_OK && (flags & 1)) {
        c0 = plat_idle_count();
        t0 = plat_ticks();
        for (done = 0; done < size / 512 && r == PX_OK; done += chunk) {
            u32 n = (size / 512 - done < chunk) ? (u32)(size / 512 - done) : chunk;
            r = px_extent_read(&e, done, g_bufmem, n);
        }
        *rt = plat_ticks() - t0;
        *ri = px_idle_x10(plat_idle_count() - c0, *rt);
    }
    fsops_bench_remove(&g_bfil);
    return r;
}

static int bench_fatfs(u64 size, u32 chunk, u32 flags, u64 *wt, u32 *wi, u64 *rt, u32 *ri)
{
    px_extent_t e;
    u64 done, t0;
    u32 c0;
    UINT bw;
    FRESULT fr = FR_OK;
    int r;

    if (chunk == 0 || chunk > g_bufmem_size)
        chunk = g_bufmem_size;
    chunk &= ~511u;
    if ((r = fsops_bench_create(&g_bfil, size, &e)) != PX_OK)
        return r;
    fill_pattern(g_bufmem, g_bufmem_size);

    c0 = plat_idle_count();
    t0 = plat_ticks();
    px_fs_lock();
    fr = f_lseek(&g_bfil, 0);
    for (done = 0; done < size && fr == FR_OK; done += chunk) {
        u32 n = (size - done < chunk) ? (u32)(size - done) : chunk;
        fr = f_write(&g_bfil, g_bufmem, n, &bw);
        if (fr == FR_OK && bw != n)
            fr = FR_DISK_ERR;
    }
    if (fr == FR_OK)
        fr = f_sync(&g_bfil);
    px_fs_unlock();
    *wt = plat_ticks() - t0;
    *wi = px_idle_x10(plat_idle_count() - c0, *wt);

    if (fr == FR_OK && (flags & 1)) {
        c0 = plat_idle_count();
        t0 = plat_ticks();
        px_fs_lock();
        fr = f_lseek(&g_bfil, 0);
        for (done = 0; done < size && fr == FR_OK; done += chunk) {
            u32 n = (size - done < chunk) ? (u32)(size - done) : chunk;
            fr = f_read(&g_bfil, g_bufmem, n, &bw);
            if (fr == FR_OK && bw != n)
                fr = FR_DISK_ERR;
        }
        px_fs_unlock();
        *rt = plat_ticks() - t0;
        *ri = px_idle_x10(plat_idle_count() - c0, *rt);
    }
    fsops_bench_remove(&g_bfil);
    return fr == FR_OK ? PX_OK : PX_EIO;
}

static u32 bench_micro(u32 *v)
{
    u32 n = 0, i;
    u64 t0;
    u32 len = g_bufmem_size / 2 < 65536 ? (g_bufmem_size / 2) & ~3u : 65536;
    volatile u32 sink = 0;

    /* v0: ticks per 1 KiB of px_fletcher over IOP RAM */
    t0 = plat_ticks();
    for (i = 0; i < 8; i++)
        sink += px_fletcher(g_bufmem, len);
    v[n++] = (u32)((plat_ticks() - t0) * 1024 / ((u64)len * 8));
    /* v1: ticks per 1 KiB of memcpy inside IOP RAM */
    t0 = plat_ticks();
    for (i = 0; i < 8; i++)
        memcpy(g_bufmem + len, g_bufmem, len);
    v[n++] = (u32)((plat_ticks() - t0) * 1024 / ((u64)len * 8));
    /* v2: free memory now */
    v[n++] = plat_total_free_mem();
    v[n++] = plat_max_free_mem();
    n += plat_bench_micro(v + n, 16 - n);
    (void)sink;
    return n;
}

int bench_run(const u8 *req, u32 len, u8 *resp, u32 *rlen)
{
    u32 kind, size_mb, chunk, flags, nvals = 0, vals[16];
    u64 size, wt = 0, rt = 0;
    u32 wi = 0, ri = 0;
    int r = PX_OK;

    if (len < 16)
        return PX_EINVAL;
    if (g_rx.active)
        return PX_EBUSY;
    kind    = rd32(req);
    size_mb = rd32(req + 4);
    chunk   = rd32(req + 8);
    flags   = rd32(req + 12);
    size    = (u64)size_mb << 20;
    memset(vals, 0, sizeof(vals));

    px_log("bench kind %u size %u MiB chunk %u\n", (unsigned)kind, (unsigned)size_mb, (unsigned)chunk);
    switch (kind) {
        case PX_BENCH_MICRO:
            nvals = bench_micro(vals);
            break;
        case PX_BENCH_DISK_RAW:
            r = (size == 0) ? PX_EINVAL : bench_raw(size, chunk, flags, &wt, &wi, &rt, &ri);
            break;
        case PX_BENCH_DISK_FATFS:
            r = (size == 0) ? PX_EINVAL : bench_fatfs(size, chunk, flags, &wt, &wi, &rt, &ri);
            break;
        case PX_BENCH_DISK_IOMANX:
            r = (size == 0) ? PX_EINVAL : plat_bench_iomanx(size, chunk, flags, &wt, &wi, &rt, &ri);
            break;
        default:
            r = PX_ENOTSUP;
    }

    memset(resp, 0, BENCH_RESP_LEN);
    wr32(resp, kind);
    wr32(resp + 4, plat_ticks_per_ms());
    wr64(resp + 8, size);
    wr64(resp + 16, wt);
    wr32(resp + 24, wi);
    wr64(resp + 32, rt);
    wr32(resp + 40, ri);
    wr32(resp + 44, nvals);
    {
        u32 i;
        for (i = 0; i < 16; i++)
            wr32(resp + 48 + i * 4, vals[i]);
    }
    *rlen = BENCH_RESP_LEN;
    px_log("bench done: %d\n", r);
    return r;
}
