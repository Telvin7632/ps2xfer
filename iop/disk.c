/*
 * Disk access: locks, bounds-checked extent I/O and the FatFs diskio glue.
 *
 * Lock order: px_fs_lock() (FatFs state) before px_ata_lock() (one ATA command
 * at a time; atad itself has no locking).
 */
#ifdef _IOP
#include "irx_imports.h"
#else
#include <string.h>
#endif
#include "px.h"
#include "ff.h"
#include "diskio.h"

static int g_ata_sem = -1, g_fs_sem = -1;

int px_disk_init(void)
{
    g_ata_sem = plat_sem_create(1, 1);
    g_fs_sem  = plat_sem_create(1, 1);
    return (g_ata_sem < 0 || g_fs_sem < 0) ? -1 : 0;
}

void px_ata_lock(void) { plat_sem_wait(g_ata_sem); }
void px_ata_unlock(void) { plat_sem_signal(g_ata_sem); }
void px_fs_lock(void) { plat_sem_wait(g_fs_sem); }
void px_fs_unlock(void) { plat_sem_signal(g_fs_sem); }

#define MAX_CMD_SECTORS 32768 /* ata_bd takes a u16 count */

int px_extent_write(const px_extent_t *e, u64 rel_sector, const void *buf, u32 count, u32 chunk)
{
    const u8 *p = (const u8 *)buf;

    /* the only way data reaches the disk outside FatFs: never outside the extent */
    if (count == 0 || rel_sector >= e->nsect || count > e->nsect - rel_sector) {
        px_log("BUG: extent write out of bounds (rel %u, count %u, nsect %u)\n",
               (unsigned)rel_sector, (unsigned)count, (unsigned)e->nsect);
        return PX_EINVAL;
    }
    if (chunk == 0 || chunk > MAX_CMD_SECTORS)
        chunk = MAX_CMD_SECTORS;
    while (count > 0) {
        u32 n = count < chunk ? count : chunk;
        int r;

        if (g_cfg.wr_boost)
            plat_writer_boost(1);
        g_stats.wr_lba = (u32)(e->lba + rel_sector);
        g_stats.wr_phase = (g_stats.wr_phase & ~0xffu) | 0x10; /* waiting for the ATA lock */
        px_ata_lock();
        g_stats.wr_phase = (g_stats.wr_phase & ~0xffu) | 0x11; /* inside the block device write */
        r = plat_bd_write(e->lba + rel_sector, p, n);
        px_ata_unlock();
        g_stats.wr_phase = (g_stats.wr_phase & ~0xffu) | 0x12;
        if (g_cfg.wr_boost)
            plat_writer_boost(0);
        if (r != 0)
            return PX_EIO;
        p += n * 512;
        rel_sector += n;
        count -= n;
    }
    return 0;
}

int px_extent_read(const px_extent_t *e, u64 rel_sector, void *buf, u32 count)
{
    u8 *p = (u8 *)buf;

    if (count == 0 || rel_sector >= e->nsect || count > e->nsect - rel_sector)
        return PX_EINVAL;
    while (count > 0) {
        u32 n = count < MAX_CMD_SECTORS ? count : MAX_CMD_SECTORS;
        int r;

        px_ata_lock();
        r = plat_bd_read(e->lba + rel_sector, p, n);
        px_ata_unlock();
        if (r != 0)
            return PX_EIO;
        p += n * 512;
        rel_sector += n;
        count -= n;
    }
    return 0;
}

/* ---------------------------------------------------------------- FatFs glue */

DSTATUS disk_status(BYTE pdrv)
{
    return (pdrv == 0 && plat_bd_sectors() != 0) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    return disk_status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    int r;

    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;
    px_ata_lock();
    r = plat_bd_read((u64)sector, buff, count);
    px_ata_unlock();
    return r == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    int r;

    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;
    px_ata_lock();
    r = plat_bd_write((u64)sector, buff, count);
    px_ata_unlock();
    return r == 0 ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (disk_status(pdrv) & STA_NOINIT)
        return RES_NOTRDY;
    switch (cmd) {
        case CTRL_SYNC:
            px_ata_lock();
            plat_bd_flush();
            px_ata_unlock();
            return RES_OK;
        case GET_SECTOR_COUNT:
            *(LBA_t *)buff = (LBA_t)plat_bd_sectors();
            return RES_OK;
        case GET_SECTOR_SIZE:
            *(WORD *)buff = 512;
            return RES_OK;
        case GET_BLOCK_SIZE:
            *(DWORD *)buff = 2048; /* 1 MiB, as the formatter */
            return RES_OK;
        default:
            return RES_PARERR;
    }
}
