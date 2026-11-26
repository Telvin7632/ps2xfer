/*
 * File system operations on the exFAT volume (FatFs R0.16), the contiguous
 * write extent of the file being received and the resume state file.
 *
 * Rules:
 *  - a received file is NAME.part, preallocated with f_expand() as ONE
 *    contiguous run; metadata is written only at OPEN_WRITE and CLOSE;
 *  - data goes into that run by raw sector writes (px_extent_write checks the
 *    bounds) or by f_write (write_path=1);
 *  - NAME.ps2x holds the resume state, updated by raw writes inside its own
 *    preallocated cluster.
 * All FatFs calls are made under px_fs_lock().
 */
#ifdef _IOP
#include "irx_imports.h"
#else
#include <string.h>
#endif
#include "px.h"
#include "ff.h"

#define STATE_MAGIC   "PX2STATE"
#define STATE_VERSION 1
#define STATE_BYTES   4096
#define MAX_PATH      256

static FATFS g_fs;
static int g_mounted;
static FIL g_part;             /* NAME.part while a transfer is open */
static px_extent_t g_state_ext;
static u32 g_state_seq;
static u8 g_fp[32];            /* source fingerprint from the PC */
static char g_final[MAX_PATH], g_part_name[MAX_PATH + 8], g_state_name[MAX_PATH + 8];
static u16 g_handle_ctr;
static u32 g_ow_flags;
static DWORD g_fattime = ((DWORD)(2026 - 1980) << 25) | (1u << 21) | (1u << 16);
static u8 g_sector[512] __attribute__((aligned(64)));

px_wfile_t g_wfile;

static void read_close(void); /* READ keeps one file open */

DWORD get_fattime(void)
{
    return g_fattime;
}

void fsops_set_time(u32 fattime)
{
    if (fattime != 0)
        g_fattime = fattime;
}

static int fr2px(FRESULT fr)
{
    switch (fr) {
        case FR_OK: return PX_OK;
        case FR_NO_FILE:
        case FR_NO_PATH: return PX_ENOENT;
        case FR_EXIST: return PX_EEXIST;
        case FR_DISK_ERR: return PX_EIO;
        case FR_INVALID_NAME: return PX_EINVAL;
        default: return PX_EFS_BASE + (int)fr;
    }
}

/* copies a NUL-terminated path from a request body and validates it */
static int get_path(const u8 *p, u32 len, char *out)
{
    u32 i;
    const char *c;

    for (i = 0; i < len && i < MAX_PATH; i++) {
        out[i] = (char)p[i];
        if (p[i] == 0)
            break;
    }
    if (i >= len || i >= MAX_PATH || i == 0 || out[0] != '/')
        return -1;
    for (c = out; *c; c++) {
        if ((u8)*c < 0x20 || *c == '\\' || *c == ':')
            return -1;
        if (c[0] == '/' && c[1] == '.' && (c[2] == '/' || c[2] == 0 || (c[2] == '.' && (c[3] == '/' || c[3] == 0))))
            return -1;
    }
    return (int)(i + 1); /* bytes consumed incl. NUL */
}

int fsops_mount(void)
{
    FRESULT fr;

    if (g_cfg.fs_iomanx || plat_bd_sectors() == 0)
        return -1;
    px_fs_lock();
    fr = f_mount(&g_fs, "", 1);
    if (fr == FR_OK) {
        g_mounted = 1;
        g_stats.flags |= PX_SF_FS_OK;
        g_stats.cluster_size = (u32)g_fs.csize * 512;
        g_stats.fs_total_mb  = (u32)(((u64)(g_fs.n_fatent - 2) * g_fs.csize) >> 11);
        px_log("exFAT mounted: type %u, cluster %u, data at LBA %u\n", (unsigned)g_fs.fs_type,
               (unsigned)g_stats.cluster_size, (unsigned)g_fs.database);
        if (g_fs.fs_type != FS_EXFAT)
            px_log("warning: the volume is not exFAT\n");
    } else {
        px_log("f_mount failed: %d\n", (int)fr);
        px_set_error("no exFAT volume on ata0");
    }
    px_fs_unlock();
    return fr == FR_OK ? 0 : -1;
}

void fsops_update_free(void)
{
    DWORD nclst;
    FATFS *fs;

    if (!g_mounted)
        return;
    px_fs_lock();
    if (f_getfree("", &nclst, &fs) == FR_OK)
        g_stats.fs_free_mb = (u32)(((u64)nclst * fs->csize) >> 11);
    px_fs_unlock();
}

/* ---------------------------------------------------------------- extents */

/* extent of an open, f_expand'ed file (absolute disk LBAs, bounds checked) */
static int file_extent(FIL *f, u64 size, px_extent_t *e)
{
    FATFS *fs = f->obj.fs;
    u64 csz = (u64)fs->csize * 512;
    DWORD clust = f->obj.sclust;
    u64 ncl = (size + csz - 1) / csz;

    if (ncl == 0)
        ncl = 1;
    if (f->obj.stat != 2 || clust < 2 || (u64)clust + ncl > fs->n_fatent)
        return -1; /* not one contiguous run */
    e->lba   = fs->database + (u64)(clust - 2) * fs->csize;
    e->nsect = ncl * fs->csize;
    if (e->lba < fs->database || e->lba + e->nsect > fs->database + (u64)(fs->n_fatent - 2) * fs->csize)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------- state file */

static u32 crc32_calc(const u8 *p, u32 n)
{
    u32 crc = 0xFFFFFFFFu;
    int k;

    while (n--) {
        crc ^= *p++;
        for (k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static int state_write(u64 durable)
{
    int r;

    memset(g_sector, 0, sizeof(g_sector));
    memcpy(g_sector, STATE_MAGIC, 8);
    wr32(g_sector + 8, STATE_VERSION);
    wr32(g_sector + 12, ++g_state_seq);
    wr64(g_sector + 16, g_rx.size);
    wr64(g_sector + 24, durable);
    memcpy(g_sector + 32, g_fp, 32);
    wr32(g_sector + 64, crc32_calc(g_sector, 64));
    /* alternate between sectors 0 and 1 so a torn write keeps the older copy */
    r = px_extent_write(&g_state_ext, g_state_seq & 1, g_sector, 1, 0);
    return r;
}

/* returns the durable offset from a valid state, or -1 */
static s64 state_read(u64 size, const u8 *fp)
{
    u32 best_seq = 0, i;
    s64 best = -1;

    for (i = 0; i < 2; i++) {
        if (px_extent_read(&g_state_ext, i, g_sector, 1) != 0)
            continue;
        if (memcmp(g_sector, STATE_MAGIC, 8) != 0 || rd32(g_sector + 8) != STATE_VERSION ||
            rd32(g_sector + 64) != crc32_calc(g_sector, 64))
            continue;
        if (rd64(g_sector + 16) != size || memcmp(g_sector + 32, fp, 32) != 0)
            continue;
        if (best < 0 || (s32)(rd32(g_sector + 12) - best_seq) > 0) {
            best_seq = rd32(g_sector + 12);
            best     = (s64)rd64(g_sector + 24);
        }
    }
    g_state_seq = best_seq;
    return best;
}

/* opens (create=1: creates and preallocates) NAME.ps2x and sets g_state_ext */
static int state_open(int create)
{
    FIL f;
    FRESULT fr;
    int r = 0;

    fr = f_open(&f, g_state_name, create ? (FA_CREATE_ALWAYS | FA_WRITE) : (FA_OPEN_EXISTING | FA_READ));
    if (fr != FR_OK)
        return fr2px(fr);
    if (create) {
        fr = f_expand(&f, STATE_BYTES, 1);
        if (fr == FR_OK)
            fr = f_sync(&f);
    }
    if (fr != FR_OK)
        r = (fr == FR_DENIED) ? PX_ENOCONTIG : fr2px(fr);
    else if (file_extent(&f, STATE_BYTES, &g_state_ext) != 0 || g_state_ext.nsect < 2)
        r = PX_EIO;
    f_close(&f);
    return r;
}

void fsops_checkpoint(void)
{
    u64 W;

    if (!g_wfile.open)
        return;
    W = g_rx.W;
    if (W < g_rx.size && W - g_rx.durable < ((u64)g_cfg.state_mb << 20))
        return;
    px_fs_lock();
    if (g_wfile.open) {
        px_ata_lock();
        plat_bd_flush();
        px_ata_unlock();
        if (state_write(W) == 0) {
            int st = plat_lock();
            g_rx.durable = W;
            plat_unlock(st);
        }
    }
    px_fs_unlock();
}

/* ---------------------------------------------------------------- writer */

int fsops_write_slot(px_slot_t *s)
{
    u32 sectors = (s->valid + 511) / 512;
    int r = 0;

    if (s->valid & 511) /* zero the tail of the last sector (inside our cluster) */
        memset(s->mem + s->valid, 0, sectors * 512 - s->valid);

    if (!g_wfile.open)
        return PX_EIO;
    if (g_cfg.write_path == 0) {
        r = px_extent_write(&g_wfile.ext, s->base >> 9, s->mem, sectors, g_cfg.ata_chunk);
    } else {
        UINT bw;
        FRESULT fr;

        px_fs_lock();
        fr = f_lseek(&g_part, s->base);
        if (fr == FR_OK)
            fr = f_write(&g_part, s->mem, s->valid, &bw);
        if (fr == FR_OK && bw != s->valid)
            fr = FR_DISK_ERR;
        px_fs_unlock();
        r = fr == FR_OK ? 0 : PX_EIO;
    }
    return r;
}

/* ---------------------------------------------------------------- commands */

static void make_parents(const char *path)
{
    char tmp[MAX_PATH];
    u32 i;

    for (i = 1; path[i] && i < MAX_PATH - 1; i++) {
        if (path[i] == '/') {
            memcpy(tmp, path, i);
            tmp[i] = 0;
            f_mkdir(tmp); /* FR_EXIST is fine */
        }
    }
}

/* waits until the writer is not inside a slot write */
static void wait_writer_idle(void)
{
    u32 i, busy;

    do {
        int st = plat_lock();
        busy = 0;
        for (i = 0; i < g_rx.B; i++)
            if (g_rx.slot[i].state == SLOT_WRITING)
                busy = 1;
        plat_unlock(st);
        if (busy)
            plat_sleep_us(2000);
    } while (busy);
}

/* closes the transfer file (keeps NAME.part and its state) */
static void close_part(int write_state)
{
    rxeng_stop();
    wait_writer_idle();
    px_fs_lock();
    if (g_wfile.open) {
        if (write_state) {
            px_ata_lock();
            plat_bd_flush();
            px_ata_unlock();
            state_write(g_rx.durable);
        }
        g_wfile.open = 0;
        f_close(&g_part);
    }
    px_fs_unlock();
}

/* body: u64 size, u32 flags, u32 reserved, u8 fp[32], path\0 */
int fsops_open_write(const u8 *req, u32 len, u8 *resp, u32 *rlen)
{
    u64 size, start = 0, credit, resume_limit;
    u32 flags;
    int r = PX_OK, n;
    FRESULT fr;
    FILINFO fno;

    if (len < 48)
        return PX_EINVAL;
    size  = rd64(req);
    flags = rd32(req + 8);
    resume_limit = (u64)rd32(req + 12) << 20; /* MiB; 0 = no limit (client checked the tail) */
    if (size == 0)
        return PX_EINVAL;

    if (g_rx.active || g_wfile.open)
        close_part(1); /* a new OPEN_WRITE replaces an interrupted one */

    g_ow_flags = flags;
    if (++g_handle_ctr == 0)
        g_handle_ctr = 1;

    if (flags & (PX_OW_SINK | PX_OW_SINK_NODMA)) {
        strcpy(g_stats.file, "(net sink)");
        rxeng_start(g_handle_ctr, size, 0, flags);
        goto reply;
    }

    if (!g_mounted)
        return PX_ENODISK;
    if ((n = get_path(req + 48, len - 48, g_final)) < 0 || strlen(g_final) + 6 > MAX_PATH)
        return PX_EINVAL;
    memcpy(g_fp, req + 16, 32);
    strcpy(g_part_name, g_final);
    strcat(g_part_name, ".part");
    strcpy(g_state_name, g_final);
    strcat(g_state_name, ".ps2x");

    px_fs_lock();
    read_close();
    if (!(flags & PX_OW_OVERWRITE) && f_stat(g_final, &fno) == FR_OK) {
        px_fs_unlock();
        return PX_EEXIST;
    }
    make_parents(g_final);

    /* resume? */
    if ((flags & PX_OW_RESUME) && f_stat(g_part_name, &fno) == FR_OK && fno.fsize == size &&
        state_open(0) == PX_OK) {
        s64 d = state_read(size, g_fp);
        if (d >= 0 && f_open(&g_part, g_part_name, FA_OPEN_EXISTING | FA_WRITE) == FR_OK) {
            if (file_extent(&g_part, size, &g_wfile.ext) == 0) {
                start = (u64)d;
                if (resume_limit != 0 && start > resume_limit)
                    start = resume_limit;
                start -= start % g_rx.S;
                g_wfile.open = 1;
                px_log("resume %s at %u MiB\n", g_final, (unsigned)(start >> 20));
            } else {
                f_close(&g_part);
            }
        }
    }

    if (!g_wfile.open) {
        f_unlink(g_part_name);
        f_unlink(g_state_name);
        fr = f_open(&g_part, g_part_name, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK) {
            fr = f_expand(&g_part, size, 1);
            if (fr == FR_OK)
                fr = f_sync(&g_part);
            if (fr != FR_OK) {
                f_close(&g_part);
                f_unlink(g_part_name);
                r = (fr == FR_DENIED) ? PX_ENOCONTIG : fr2px(fr);
            } else if (file_extent(&g_part, size, &g_wfile.ext) != 0) {
                f_close(&g_part);
                f_unlink(g_part_name);
                r = PX_EIO;
            }
        } else {
            r = fr2px(fr);
        }
        if (r == PX_OK) {
            g_state_seq = 0;
            r = state_open(1);
            if (r != PX_OK) {
                f_close(&g_part);
                f_unlink(g_part_name);
                f_unlink(g_state_name);
            }
        }
        if (r != PX_OK) {
            px_fs_unlock();
            px_log("open_write %s failed: %d\n", g_final, r);
            return r;
        }
        g_wfile.open = 1;
        px_log("open %s: %u MiB at LBA %u (%u sectors)\n", g_final, (unsigned)(size >> 20),
               (unsigned)g_wfile.ext.lba, (unsigned)g_wfile.ext.nsect);
        if (g_wfile.ext.lba & 7)
            px_log("warning: file start LBA is not 4K-aligned\n");
    }
    g_wfile.fil = &g_part;
    px_fs_unlock();

    px_strlcpy(g_stats.file, g_final, sizeof(g_stats.file));
    rxeng_start(g_handle_ctr, size, start, flags);
    if (!(flags & PX_OW_RESUME) || start == 0) {
        px_fs_lock();
        state_write(0);
        px_fs_unlock();
    }

reply:
    credit = start + (u64)g_rx.B * g_rx.S;
    if (credit > size)
        credit = size;
    wr16(resp, g_handle_ctr);
    wr16(resp + 2, 0);
    wr64(resp + 4, start);
    wr64(resp + 12, credit);
    wr32(resp + 20, g_rx.S);
    wr32(resp + 24, g_rx.B);
    wr64(resp + 28, g_wfile.open ? g_wfile.ext.lba : 0);
    *rlen = 36;
    return PX_OK;
}

/* body: u16 handle, u16 reserved, u32 reserved, u64 manifest */
int fsops_close(const u8 *req, u32 len, u8 *resp, u32 *rlen)
{
    u64 manifest;
    FRESULT fr;
    int r = PX_OK;

    if (len < 16)
        return PX_EINVAL;
    if (rd16(req) != g_rx.handle || (!g_rx.active && !g_wfile.open))
        return PX_EHANDLE;
    if (g_rx.error)
        return g_rx.error;
    if (g_rx.W < g_rx.size)
        return PX_EINCOMPLETE;
    manifest = rd64(req + 8);
    wr64(resp, g_rx.manifest);
    *rlen = 8;
    if (manifest != g_rx.manifest) {
        px_log("CLOSE: manifest mismatch\n");
        close_part(1);
        return PX_EMISMATCH;
    }

    if (g_ow_flags & (PX_OW_SINK | PX_OW_SINK_NODMA)) {
        rxeng_stop();
        g_stats.file[0] = 0;
        return PX_OK;
    }

    rxeng_stop();
    wait_writer_idle();
    px_fs_lock();
    px_ata_lock();
    plat_bd_flush();
    px_ata_unlock();
    g_wfile.open = 0;
    fr = f_close(&g_part);
    if (fr == FR_OK)
        f_unlink(g_state_name);
    if (fr == FR_OK && !(g_ow_flags & PX_OW_KEEP_PART)) {
        if (g_ow_flags & PX_OW_OVERWRITE)
            f_unlink(g_final);
        fr = f_rename(g_part_name, g_final);
    }
    px_fs_unlock();
    if (fr != FR_OK)
        r = fr2px(fr);
    px_log("close %s: %d\n", g_final, r);
    g_stats.file[0] = 0;
    fsops_update_free();
    return r;
}

/* body: u16 handle, u8 delete; NULL = abort whatever is open (EXIT) */
int fsops_abort(const u8 *req, u32 len)
{
    int del = (req != NULL && len >= 3) ? req[2] : 0;

    if (req != NULL && len >= 2 && rd16(req) != g_rx.handle)
        return PX_EHANDLE;
    if (g_ow_flags & (PX_OW_SINK | PX_OW_SINK_NODMA)) {
        rxeng_stop();
        return PX_OK;
    }
    close_part(!del);
    if (del) {
        px_fs_lock();
        f_unlink(g_part_name);
        f_unlink(g_state_name);
        px_fs_unlock();
    }
    g_stats.file[0] = 0;
    px_log("abort %s%s\n", g_final, del ? " (deleted)" : "");
    return PX_OK;
}

/* body: u32 cursor, path\0 -> u32 next (0 = end), u16 count, u16 0, entries:
   u64 size, u32 fattime, u8 attr, u8 namelen, name */
int fsops_list(const u8 *req, u32 len, u8 *resp, u32 *rlen)
{
    char path[MAX_PATH];
    DIR dir;
    FILINFO fno;
    u32 cursor, idx = 0, pos = 8, count = 0, next = 0;
    FRESULT fr;

    if (len < 5 || get_path(req + 4, len - 4, path) < 0)
        return PX_EINVAL;
    if (!g_mounted)
        return PX_ENODISK;
    cursor = rd32(req);
    px_fs_lock();
    fr = f_opendir(&dir, path);
    while (fr == FR_OK) {
        u32 nl;
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == 0)
            break;
        if (idx++ < cursor)
            continue;
        nl = (u32)strlen(fno.fname);
        if (nl > 255)
            nl = 255;
        if (pos + 14 + nl > PX_MBOX_SIZE - PX_CTL_HDR - 64) {
            next = idx - 1;
            break;
        }
        wr64(resp + pos, (u64)fno.fsize);
        wr32(resp + pos + 8, ((u32)fno.fdate << 16) | fno.ftime);
        resp[pos + 12] = fno.fattrib;
        resp[pos + 13] = (u8)nl;
        memcpy(resp + pos + 14, fno.fname, nl);
        pos += 14 + nl;
        count++;
    }
    f_closedir(&dir);
    px_fs_unlock();
    if (fr != FR_OK)
        return fr2px(fr);
    wr32(resp, next);
    wr16(resp + 4, (u16)count);
    wr16(resp + 6, 0);
    *rlen = pos;
    return PX_OK;
}

/* path -> u64 size, u32 fattime, u8 attr, u8 contiguous, u8[2] 0 */
int fsops_stat(const u8 *req, u32 len, u8 *resp, u32 *rlen)
{
    char path[MAX_PATH];
    FILINFO fno;
    FRESULT fr;

    if (get_path(req, len, path) < 0)
        return PX_EINVAL;
    if (!g_mounted)
        return PX_ENODISK;
    memset(resp, 0, 16);
    px_fs_lock();
    fr = f_stat(path, &fno);
    if (fr == FR_OK && !(fno.fattrib & AM_DIR)) {
        FIL f;
        /* resp[13]: 1 = one contiguous run (what OPL wants), 0 = fragmented/FAT chain */
        if (f_open(&f, path, FA_READ) == FR_OK) {
            resp[13] = (f.obj.stat == 2 || fno.fsize == 0) ? 1 : 0;
            f_close(&f);
        }
    }
    px_fs_unlock();
    if (fr != FR_OK)
        return fr2px(fr);
    wr64(resp, (u64)fno.fsize);
    wr32(resp + 8, ((u32)fno.fdate << 16) | fno.ftime);
    resp[12] = fno.fattrib;
    *rlen = 16;
    return PX_OK;
}

int fsops_mkdir(const u8 *req, u32 len)
{
    char path[MAX_PATH];
    FRESULT fr;

    if (get_path(req, len, path) < 0)
        return PX_EINVAL;
    if (!g_mounted)
        return PX_ENODISK;
    px_fs_lock();
    make_parents(path);
    fr = f_mkdir(path);
    px_fs_unlock();
    return fr2px(fr);
}

int fsops_delete(const u8 *req, u32 len)
{
    char path[MAX_PATH];
    FRESULT fr;

    if (get_path(req, len, path) < 0)
        return PX_EINVAL;
    if (!g_mounted)
        return PX_ENODISK;
    if (g_wfile.open && (strcmp(path, g_part_name) == 0 || strcmp(path, g_state_name) == 0)) {
        /* a file of the open transfer: refused while its data still arrives;
           an interrupted one (silent sender) is closed first. Deleting the
           .part takes its resume state with it. */
        if (rxeng_busy())
            return PX_EBUSY;
        if (strcmp(path, g_part_name) == 0) {
            u8 a[3];
            wr16(a, g_rx.handle);
            a[2] = 1; /* delete .part and .ps2x */
            fsops_abort(a, sizeof(a));
            fsops_update_free();
            return PX_OK;
        }
        close_part(0);
        g_stats.file[0] = 0;
    }
    px_fs_lock();
    read_close();
    fr = f_unlink(path);
    px_fs_unlock();
    if (fr == FR_DENIED)
        return PX_ENOTEMPTY;
    if (fr == FR_OK)
        fsops_update_free();
    return fr2px(fr);
}

int fsops_rename(const u8 *req, u32 len)
{
    char from[MAX_PATH], to[MAX_PATH];
    int n;
    FRESULT fr;

    if ((n = get_path(req, len, from)) < 0 || get_path(req + n, len - (u32)n, to) < 0)
        return PX_EINVAL;
    if (!g_mounted)
        return PX_ENODISK;
    if (g_wfile.open && (strcmp(from, g_part_name) == 0 || strcmp(from, g_state_name) == 0))
        return PX_EBUSY;
    px_fs_lock();
    read_close();
    make_parents(to);
    fr = f_rename(from, to);
    px_fs_unlock();
    return fr2px(fr);
}

/* -> u64 total, u64 free, u32 cluster */
int fsops_df(u8 *resp, u32 *rlen)
{
    DWORD nclst;
    FATFS *fs;
    FRESULT fr;

    if (!g_mounted)
        return PX_ENODISK;
    px_fs_lock();
    fr = f_getfree("", &nclst, &fs);
    px_fs_unlock();
    if (fr != FR_OK)
        return fr2px(fr);
    wr64(resp, (u64)(fs->n_fatent - 2) * fs->csize * 512);
    wr64(resp + 8, (u64)nclst * fs->csize * 512);
    wr32(resp + 16, (u32)fs->csize * 512);
    g_stats.fs_free_mb = (u32)(((u64)nclst * fs->csize) >> 11);
    *rlen = 20;
    return PX_OK;
}

/* body: u64 offset (multiple of 1 MiB), u32 count, path\0
   -> u32 count, u32 sums[] (word-Fletcher of each 1 MiB block, the last one
   zero-padded to 4 bytes) */
/* ---------------------------------------------------------------- screen progress
 * Verify and read have no OPEN/CLOSE of their own: each request refreshes
 * the progress, and fsops_task_expire() (ack thread, periodic) clears it
 * 3 s after the last one. The file fields belong to a write while one runs. */
static u32 g_task_ms;

static void task_progress(u32 flag, const char *path, u64 size, u64 done)
{
    if (g_stats.flags & PX_SF_XFER)
        return;
    if (!(g_stats.flags & flag) || strcmp(g_stats.file, path) != 0)
        px_strlcpy(g_stats.file, path, sizeof(g_stats.file));
    g_stats.file_size_lo = (u32)size;
    g_stats.file_size_hi = (u32)(size >> 32);
    g_stats.written_lo   = (u32)done;
    g_stats.written_hi   = (u32)(done >> 32);
    g_task_ms = px_ms();
    {
        int st = plat_lock();
        g_stats.flags = (g_stats.flags & ~(PX_SF_VERIFY | PX_SF_READ)) | flag;
        plat_unlock(st);
    }
}

void fsops_task_expire(void)
{
    if ((g_stats.flags & (PX_SF_VERIFY | PX_SF_READ)) && px_ms() - g_task_ms > 3000) {
        int st = plat_lock();
        g_stats.flags &= ~(PX_SF_VERIFY | PX_SF_READ);
        if (!(g_stats.flags & PX_SF_XFER))
            g_stats.file[0] = 0;
        plat_unlock(st);
    }
}

/* ---------------------------------------------------------------- verify read-ahead
 * The writer thread is idle while no write runs: it reads the next chunk of
 * a contiguous file while the control thread checksums the current one.
 * Measured on hardware this overlaps little: ps2dev9 spins the CPU during
 * the ATA DMA, so disk time and checksum time still mostly add up
 * (verify ~18-19 MB/s; the unrolled Fletcher brought most of the gain). */
static struct {
    const px_extent_t *e;
    u64 rel;
    void *buf;
    u32 count;
    volatile int pending;
    int r;
} g_ra;
static int g_ra_done = -1;

void fsops_readahead_service(void) /* writer thread */
{
    if (!g_ra.pending)
        return;
    g_ra.r       = px_extent_read(g_ra.e, g_ra.rel, g_ra.buf, g_ra.count);
    g_ra.pending = 0;
    plat_sem_signal(g_ra_done);
}

static void ra_start(const px_extent_t *e, u64 byte_off, void *buf, u32 bytes)
{
    g_ra.e       = e;
    g_ra.rel     = byte_off >> 9;
    g_ra.buf     = buf;
    g_ra.count   = (bytes + 511) >> 9;
    g_ra.pending = 1;
    plat_sem_signal(g_writer_sem);
}

static int ra_wait(void)
{
    plat_sem_wait(g_ra_done);
    return g_ra.r;
}

/* BLOCKSUMS of a contiguous file, double-buffered; returns blocks done or -PX_E* */
static int blocksums_contig(FIL *f, const char *path, u64 off, u32 count, u8 *resp)
{
    px_extent_t e;
    u64 fsize = f_size(f), end;
    u32 ch, per_block, nch, k, blk = 0;
    u8 *buf[2];
    px_fletcher_t fl;

    if (g_bufmem_size >= (1u << 20))
        ch = 512u << 10;
    else if (g_bufmem_size >= (512u << 10))
        ch = 256u << 10;
    else
        return -PX_ENOTSUP;
    if (count == 0 || (off & 511) || off >= fsize || file_extent(f, fsize, &e) != 0)
        return -PX_ENOTSUP;
    if (g_ra_done < 0 && (g_ra_done = plat_sem_create(0, 1)) < 0)
        return -PX_ENOTSUP;
    end = off + ((u64)count << 20);
    if (end > fsize)
        end = fsize;
    per_block = (1u << 20) / ch;
    nch       = (u32)((end - off + ch - 1) / ch);
    buf[0]    = g_bufmem;
    buf[1]    = g_bufmem + ch;

    px_fletcher_init(&fl);
    ra_start(&e, off, buf[0], (u32)(end - off < ch ? end - off : ch));
    for (k = 0; k < nch; k++) {
        u64 pos = off + (u64)k * ch;
        u32 len = (u32)(end - pos < ch ? end - pos : ch);
        u8 *b   = buf[k & 1];

        if (ra_wait() != 0)
            return -PX_EIO;
        if (k + 1 < nch) {
            u64 np = pos + ch;
            ra_start(&e, np, buf[(k + 1) & 1], (u32)(end - np < ch ? end - np : ch));
        }
        if (len & 3)
            memset(b + len, 0, 4 - (len & 3)); /* inside the buffer: len < ch here */
        px_fletcher_update(&fl, b, (len + 3) & ~3u);
        if ((k + 1) % per_block == 0 || k + 1 == nch) {
            wr32(resp + 4 + blk * 4, px_fletcher_final(&fl));
            blk++;
            px_fletcher_init(&fl);
            task_progress(PX_SF_VERIFY, path, fsize, pos + len);
        }
    }
    return (int)blk;
}

/* ---------------------------------------------------------------- verify on the EE
 * IOP: read 512 KiB from the disk, hand it to the EE (SIF command + DMA),
 * read the next one meanwhile; the EE sums at a fraction of the IOP's cost.
 * The first block after boot is also summed here and compared, so a broken
 * EE path disables itself instead of reporting false mismatches. */
static u32 g_ee_seq;
static int g_ee_checked;

static int ee_wait_dma(u32 trid)
{
    u32 t;

    for (t = 0; t < 20000; t++) { /* 2 s */
        if (plat_ee_dma_done(trid))
            return 0;
        plat_sleep_us(100);
    }
    return -1;
}

static int ee_wait_result(u32 seq, u32 *csum, u32 *flags)
{
    u32 t;

    for (t = 0; t < 20000; t++) {
        if (plat_ee_sum_result(seq, csum, flags))
            return 0;
        plat_sleep_us(100);
    }
    return -1;
}

static int blocksums_ee(FIL *f, const char *path, u64 off, u32 count, u8 *resp)
{
    const u32 ch = PX_SUM_CHUNK, per_block = (1u << 20) / PX_SUM_CHUNK;
    px_extent_t e;
    u64 fsize = f_size(f), end;
    u32 nch, k, j, blk = 0, base, trid[2] = {0, 0}, sent = 0, got = 0;
    u8 *ib[2];
    px_fletcher_t chk;
    int err = 0;

    if (!plat_ee_sum_ready() || g_bufmem_size < 2 * ch)
        return -PX_ENOTSUP;
    if (count == 0 || (off & 511) || off >= fsize || file_extent(f, fsize, &e) != 0)
        return -PX_ENOTSUP;
    end = off + ((u64)count << 20);
    if (end > fsize)
        end = fsize;
    nch   = (u32)((end - off + ch - 1) / ch);
    ib[0] = g_bufmem;
    ib[1] = g_bufmem + ch;
    base  = g_ee_seq;
    g_ee_seq += nch;
    px_fletcher_init(&chk);

    for (k = 0; k < nch && !err; k++) {
        u32 b = k & 1, flags = 0, csum, rflags;
        u64 pos = off + (u64)k * ch;
        u32 len = (u32)(end - pos < ch ? end - pos : ch);

        /* IOP buffer b is free once chunk k-2 left it */
        if (k >= 2 && ee_wait_dma(trid[b]) != 0) {
            err = -PX_ENOTSUP;
            break;
        }
        if (px_extent_read(&e, pos >> 9, ib[b], (len + 511) >> 9) != 0) {
            err = -PX_EIO;
            break;
        }
        /* EE buffer b is free once the EE answered chunk k-2 */
        if (k >= 2) {
            j = k - 2;
            if (ee_wait_result(base + j, &csum, &rflags) != 0) {
                err = -PX_ENOTSUP;
                break;
            }
            got++;
            if (rflags & PX_SUM_LAST) {
                wr32(resp + 4 + blk * 4, csum);
                blk++;
            }
        }
        if (k % per_block == 0)
            flags |= PX_SUM_FIRST;
        if ((k + 1) % per_block == 0 || k + 1 == nch)
            flags |= PX_SUM_LAST;
        if (!(trid[b] = plat_ee_sum_send(base + k, (int)b, ib[b], len, flags))) {
            err = -PX_ENOTSUP;
            break;
        }
        sent++;
        if (!g_ee_checked && k < per_block) /* the zeroed tail is part of the sum */
            px_fletcher_update(&chk, ib[b], (len + 3) & ~3u);
        task_progress(PX_SF_VERIFY, path, fsize, pos + len);
    }
    /* collect what is still in flight (also after an error: buffers are reused) */
    for (j = got; j < sent; j++) {
        u32 csum, rflags;

        if (ee_wait_result(base + j, &csum, &rflags) != 0) {
            err = -PX_ENOTSUP;
            break;
        }
        if (!err && (rflags & PX_SUM_LAST)) {
            wr32(resp + 4 + blk * 4, csum);
            blk++;
        }
    }
    for (j = 0; j < 2; j++)
        if (trid[j] && ee_wait_dma(trid[j]) != 0)
            err = -PX_ENOTSUP;
    if (err == -PX_ENOTSUP) {
        px_log("EE verify helper not answering: verify on the IOP from now on\n");
        plat_ee_sum_disable();
    }
    if (err)
        return err;
    if (!g_ee_checked && blk > 0) {
        u32 mine = px_fletcher_final(&chk);
        if (mine != rd32(resp + 4)) {
            px_log("EE verify helper: sum %08x, IOP %08x: disabled\n", (unsigned)rd32(resp + 4), (unsigned)mine);
            plat_ee_sum_disable();
            return -PX_ENOTSUP;
        }
        px_log("EE verify helper: first block checked against the IOP\n");
        g_ee_checked = 1;
    }
    return (int)blk;
}

int fsops_blocksums(const u8 *req, u32 len, u8 *resp, u32 *rlen)
{
    char path[MAX_PATH];
    FIL f;
    FRESULT fr;
    u64 off;
    u32 count, i, maxn = (PX_MBOX_SIZE - PX_CTL_HDR - 64 - 4) / 4;
    u32 chunk = g_bufmem_size < (1u << 20) ? g_bufmem_size : (1u << 20);

    if (len < 13 || get_path(req + 12, len - 12, path) < 0)
        return PX_EINVAL;
    if (!g_mounted)
        return PX_ENODISK;
    if (g_rx.active)
        return PX_EBUSY; /* the write buffers are used as read buffer */
    off   = rd64(req);
    count = rd32(req + 8);
    if (count > maxn)
        count = maxn;
    chunk &= ~(u32)4095;

    px_fs_lock();
    fr = f_open(&f, path, FA_READ);
    if (fr == FR_OK) {
        int n = blocksums_ee(&f, path, off, count, resp);
        if (n == -PX_ENOTSUP)
            n = blocksums_contig(&f, path, off, count, resp);
        if (n != -PX_ENOTSUP) {
            f_close(&f);
            px_fs_unlock();
            if (n < 0)
                return -n;
            wr32(resp, (u32)n);
            *rlen = 4 + (u32)n * 4;
            return PX_OK;
        }
        if (off == 0)
            px_log("verify %s: not contiguous, reading through FatFs\n", path);
        fr = f_lseek(&f, off); /* fragmented (copied by another driver): read through FatFs */
    }
    for (i = 0; fr == FR_OK && i < count; i++) {
        px_fletcher_t fl;
        u32 left = 1u << 20, got_total = 0;

        px_fletcher_init(&fl);
        while (left > 0) {
            UINT br;
            u32 want = left < chunk ? left : chunk;
            fr = f_read(&f, g_bufmem, want, &br);
            if (fr != FR_OK || br == 0)
                break;
            if (br & 3)
                memset(g_bufmem + br, 0, 4 - (br & 3));
            px_fletcher_update(&fl, g_bufmem, (br + 3) & ~3u);
            got_total += br;
            left -= br;
            if (br < want)
                break;
        }
        if (fr != FR_OK || got_total == 0)
            break;
        wr32(resp + 4 + i * 4, px_fletcher_final(&fl));
        task_progress(PX_SF_VERIFY, path, f_size(&f), off + (u64)i * (1u << 20) + got_total);
    }
    f_close(&f);
    px_fs_unlock();
    if (fr != FR_OK)
        return fr2px(fr);
    wr32(resp, i);
    *rlen = 4 + i * 4;
    return PX_OK;
}

/* DISKINFO (read-only): raw sector 0 (MBR), raw sector 1 (GPT header if
   any), u64 start LBA of the mounted volume, u32 FatFs type, u32 0 */
int fsops_diskinfo(u8 *resp, u32 *rlen)
{
    int r;

    if (plat_bd_sectors() == 0)
        return PX_ENODISK;
    px_fs_lock();
    px_ata_lock();
    r = plat_bd_read(0, g_sector, 1);
    if (r == 0)
        memcpy(resp, g_sector, 512);
    if (r == 0)
        r = plat_bd_read(1, g_sector, 1);
    if (r == 0)
        memcpy(resp + 512, g_sector, 512);
    px_ata_unlock();
    wr64(resp + 1024, g_mounted ? (u64)g_fs.volbase : 0);
    wr32(resp + 1032, g_mounted ? g_fs.fs_type : 0);
    wr32(resp + 1036, 0);
    px_fs_unlock();
    *rlen = 1040;
    return r == 0 ? PX_OK : PX_EIO;
}

/* RESUME_INFO: body u64 size, u32 0, u32 0, u8 fp[32], path\0 ->
   u64 durable offset of path.part (0 = nothing to resume), u64 0.
   An open transfer is closed resumably first, so BLOCKSUMS can read
   path.part afterwards. */
int fsops_resume_info(const u8 *req, u32 len, u8 *resp, u32 *rlen)
{
    char path[MAX_PATH];
    FILINFO fno;
    s64 d = -1;
    u64 size;

    if (len < 49 || get_path(req + 48, len - 48, path) < 0 || strlen(path) + 6 > MAX_PATH)
        return PX_EINVAL;
    if (!g_mounted)
        return PX_ENODISK;
    if (g_rx.active || g_wfile.open)
        close_part(1);
    size = rd64(req);
    strcpy(g_part_name, path);
    strcat(g_part_name, ".part");
    strcpy(g_state_name, path);
    strcat(g_state_name, ".ps2x");
    px_fs_lock();
    if (f_stat(g_part_name, &fno) == FR_OK && fno.fsize == size && state_open(0) == PX_OK)
        d = state_read(size, req + 16);
    px_fs_unlock();
    wr64(resp, d > 0 ? (u64)d : 0);
    wr64(resp + 8, 0);
    *rlen = 16;
    return PX_OK;
}

/* READ: body u64 offset, u32 length (<= PX_READ_MAX), path\0.
   The data is sent as READ_DATA packets (DATA layout: offset, pkt_seq =
   req_id, csum 0, handle 0, len; LAST on the final one) before the response:
   u64 file size, u32 bytes read. Missing packets: the client asks again. */
static FIL g_rfil;
static char g_rpath[MAX_PATH];
static int g_ropen;

int fsops_read(const u8 *req, u32 len, u32 req_id, u8 *resp, u32 *rlen)
{
    char path[MAX_PATH];
    u64 off, fsize = 0;
    u32 want, got = 0, pos;
    FRESULT fr = FR_OK;
    UINT br;
    u8 h[PX_DATA_HDR] __attribute__((aligned(4)));

    if (len < 13 || get_path(req + 12, len - 12, path) < 0)
        return PX_EINVAL;
    if (!g_mounted)
        return PX_ENODISK;
    if (g_rx.active || g_bufmem == NULL)
        return PX_EBUSY; /* the write buffers are the read buffer */
    off  = rd64(req);
    want = rd32(req + 8);
    if (want > PX_READ_MAX)
        want = PX_READ_MAX;
    if (want > g_bufmem_size)
        want = g_bufmem_size - (g_bufmem_size % PX_PAYLOAD);

    px_fs_lock();
    if (!g_ropen || strcmp(path, g_rpath) != 0) { /* keep the file open between requests */
        if (g_ropen)
            f_close(&g_rfil);
        g_ropen = 0;
        fr = f_open(&g_rfil, path, FA_READ);
        if (fr == FR_OK) {
            g_ropen = 1;
            strcpy(g_rpath, path);
        }
    }
    if (fr == FR_OK) {
        fsize = f_size(&g_rfil);
        if (off < fsize) {
            if (want > fsize - off)
                want = (u32)(fsize - off);
            fr = f_lseek(&g_rfil, off);
            if (fr == FR_OK)
                fr = f_read(&g_rfil, g_bufmem, want, &br);
            if (fr == FR_OK)
                got = br;
        }
    }
    px_fs_unlock();
    if (fr != FR_OK) {
        if (g_ropen)
            f_close(&g_rfil);
        g_ropen = 0;
        return fr2px(fr);
    }

    for (pos = 0; pos < got; pos += PX_PAYLOAD) {
        u32 n = got - pos < PX_PAYLOAD ? got - pos : PX_PAYLOAD;
        if (n & 3)
            memset(g_bufmem + pos + n, 0, 4 - (n & 3));
        h[0] = PX_T_READ_DATA;
        h[1] = (pos + n >= got) ? PX_DF_LAST : 0;
        wr64(h + 2, off + pos);
        wr32(h + 10, req_id);
        wr32(h + 14, 0);
        wr16(h + 18, 0);
        wr16(h + 20, (u16)n);
        plat_send_data(h, g_bufmem + pos, n);
    }
    task_progress(PX_SF_READ, path, fsize, off + got);
    wr64(resp, fsize);
    wr32(resp + 8, got);
    *rlen = 12;
    return PX_OK;
}

/* closes the file kept open by READ (before writes/deletes touch it) */
static void read_close(void)
{
    if (g_ropen) {
        f_close(&g_rfil);
        g_ropen = 0;
    }
}

/* ---------------------------------------------------------------- bench helpers */

/* creates /.ps2xfer/bench.bin preallocated to `size`; opens it in *f */
int fsops_bench_create(void *fil, u64 size, px_extent_t *e)
{
    FIL *f = (FIL *)fil;
    FRESULT fr;

    if (!g_mounted)
        return PX_ENODISK;
    px_fs_lock();
    f_mkdir("/.ps2xfer");
    fr = f_open(f, "/.ps2xfer/bench.bin", FA_CREATE_ALWAYS | FA_WRITE | FA_READ);
    if (fr == FR_OK) {
        fr = f_expand(f, size, 1);
        if (fr == FR_OK)
            fr = f_sync(f);
        if (fr != FR_OK)
            f_close(f);
    }
    if (fr == FR_OK && file_extent(f, size, e) != 0) {
        f_close(f);
        fr = FR_INT_ERR;
    }
    px_fs_unlock();
    if (fr != FR_OK) {
        px_fs_lock();
        f_unlink("/.ps2xfer/bench.bin");
        px_fs_unlock();
    }
    return fr == FR_DENIED ? PX_ENOCONTIG : fr2px(fr);
}

void fsops_bench_remove(void *fil)
{
    px_fs_lock();
    f_close((FIL *)fil);
    f_unlink("/.ps2xfer/bench.bin");
    px_fs_unlock();
    fsops_update_free();
}
