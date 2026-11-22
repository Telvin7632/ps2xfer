/*
 * ps2xfer core: shared declarations of the portable part
 * (rxeng.c, writer.c, ctrl.c, fsops.c, bench.c, util.c, log.c).
 */
#ifndef PX_H
#define PX_H

#include "plat.h"
#include "proto.h"
#include "px_stats.h"

#define PX_VERSION_STR "0.1.0"

#define PX_MAX_BUFS      8
#define PX_MAX_BUF_UNITS 16  /* 16 * 45056 = 704 KiB per buffer */
#define PX_MAX_SLOT_PKTS (PX_MAX_BUF_UNITS * PX_UNIT_PKTS) /* 512 */

/* ---------------------------------------------------------------- config */
typedef struct px_cfg {
    u32 ip;            /* host order a.b.c.d */
    u32 port;
    u32 bufs;          /* B */
    u32 buf_units;     /* k: buffer = k * PX_UNIT */
    u32 ata_chunk;     /* sectors per bd write; 0 = whole buffer */
    u32 csum;          /* 0 off, 1 every packet, N = every N-th packet */
    u32 ack_every;     /* packets */
    u32 ack_ms;
    u32 hdr_dma;       /* 1: read the 64-byte header by DMA instead of PIO */
    u32 dma_block;     /* RX payload DMA block: 64, 128, 512, 0 = auto */
    u32 write_path;    /* 0 raw extent writes, 1 FatFs f_write */
    u32 pause;         /* 1: advertise/enable 802.3x PAUSE */
    u32 rx_prio;       /* SMAP thread priority */
    u32 prof;          /* 1: per-packet cycle profiling */
    u32 state_mb;      /* resume-state update interval, MiB */
    u32 fs_iomanx;     /* 1: bdmfs_fatfs owns the volume; our FatFs off */
    u32 wr_boost;      /* 1: each ATA write chunk runs above the RX thread;
                          use with a small ata_chunk (FIFO holds ~1.3 ms) */
} px_cfg_t;

extern px_cfg_t g_cfg;
extern px_stats_t g_stats;

void px_cfg_defaults(px_cfg_t *c);
/* "key=value"; returns 0 if applied, -1 if unknown/invalid. live=1: only
   keys that may change while running are accepted. */
int px_cfg_set(px_cfg_t *c, const char *kv, int live);
int px_parse_ip(const char *s, u32 *ip);

/* ---------------------------------------------------------------- util */
u32 px_fletcher(const void *buf, u32 len); /* len multiple of 4 */
typedef struct px_fletcher { u32 s1, s2; } px_fletcher_t;
void px_fletcher_init(px_fletcher_t *f);
void px_fletcher_update(px_fletcher_t *f, const void *buf, u32 len);
u32 px_fletcher_final(const px_fletcher_t *f);
/* CPU idle measurement (idle thread counter) */
void px_idle_calibrate(u32 ms);
u32 px_idle_x10(u32 count_delta, u64 ticks_delta); /* idle per mille */
void px_load_update(void);                         /* refreshes g_stats.cpu_load_x10 */
static inline u16 rd16(const u8 *p) { return (u16)(p[0] | (p[1] << 8)); }
static inline u32 rd32(const u8 *p) { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline u64 rd64(const u8 *p) { return (u64)rd32(p) | ((u64)rd32(p + 4) << 32); }
static inline void wr16(u8 *p, u16 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
static inline void wr32(u8 *p, u32 v) { p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24); }
static inline void wr64(u8 *p, u64 v) { wr32(p, (u32)v); wr32(p + 4, (u32)(v >> 32)); }
u32 px_ms(void);   /* ms since start */
void px_strlcpy(char *dst, const char *src, u32 size); /* truncates, always terminates */
void px_set_error(const char *msg);

/* ---------------------------------------------------------------- log ring */
void px_log_init(void);
void px_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* copies log text starting at *cursor; returns bytes copied, updates cursor */
u32 px_log_read(u32 *cursor, char *dst, u32 max, u32 *lost);

/* ---------------------------------------------------------------- disk access */
int px_disk_init(void);           /* creates locks */
void px_ata_lock(void);
void px_ata_unlock(void);
void px_fs_lock(void);
void px_fs_unlock(void);

/* extent of a preallocated contiguous file (absolute disk LBAs) */
typedef struct px_extent {
    u64 lba;
    u64 nsect;
} px_extent_t;
/* bounds-checked raw write/read inside an extent; 0 or -PX_E* */
int px_extent_write(const px_extent_t *e, u64 rel_sector, const void *buf, u32 count, u32 chunk);
int px_extent_read(const px_extent_t *e, u64 rel_sector, void *buf, u32 count);

/* ---------------------------------------------------------------- receive engine */
enum { SLOT_IDLE = 0, SLOT_FILLING, SLOT_FULL, SLOT_WRITING, SLOT_DONE };

typedef struct px_slot {
    u8 *mem;
    u64 base;      /* file offset of mem[0] */
    u32 valid;     /* bytes of file data in this slot */
    u16 npkts;
    u16 rcvd;
    u8 state;
    u32 bitmap[PX_MAX_SLOT_PKTS / 32];
} px_slot_t;

typedef struct px_rx {
    int active;
    u16 handle;
    u32 flags;         /* PX_OW_* */
    u64 size;
    u32 S;             /* bytes per slot */
    u32 B;             /* slots */
    u32 P;             /* packets per slot = S / PX_PAYLOAD */
    u64 W;             /* base of the oldest slot = written watermark */
    u32 W_idx;         /* slot index of W */
    u32 W_pkt;         /* W / PX_PAYLOAD (avoids 64-bit division per packet) */
    u64 durable;
    u32 max_pkt_seq;
    u32 pkts_since_ack;
    u32 last_ack_ms;
    u32 seen_pkts;     /* g_stats.rx_pkts at the last ack-thread look */
    u32 last_data_ms;  /* when DATA last arrived (ack-thread resolution) */
    int credit_changed;
    u64 manifest;
    int error;         /* PX_E* that stopped the session, 0 = ok */
    px_slot_t slot[PX_MAX_BUFS];
} px_rx_t;

extern px_rx_t g_rx;
extern u8 *g_bufmem;   /* B*S bytes, allocated once */
extern u32 g_bufmem_size;

int rxeng_alloc(void);  /* allocates write buffers per g_cfg and free memory */
/* start a session at file offset `start` (multiple of S) */
void rxeng_start(u16 handle, u64 size, u64 start, u32 flags);
void rxeng_stop(void);

/* DATA packet. hdr points to the 22-byte DATA header (4-aligned at +2).
   Returns 1 if an ACK should be sent now. */
int rxeng_data(const u8 *hdr, plat_payload_reader_t rd, void *ctx);
/* builds an ACK into buf (>= PX_ACK_HDR + 256); returns length */
u32 rxeng_build_ack(u8 *buf);
/* sends an ACK if one is due (force = send anyway) */
void rxeng_maybe_ack(int force);
int rxeng_busy(void);
void fsops_task_expire(void);

/* ---------------------------------------------------------------- writer */
extern int g_writer_sem;   /* signalled when a slot becomes FULL */
void writer_thread(void *arg);
/* the writer calls these to dispose of a slot */
typedef struct px_wfile {
    int open;
    px_extent_t ext;
    void *fil;         /* FatFs FIL* for write_path=1 */
} px_wfile_t;
extern px_wfile_t g_wfile;

/* ---------------------------------------------------------------- control */
#define PX_MBOX_SLOTS 8
#define PX_MBOX_SIZE  1536
int ctrl_init(void);
/* called from the RX path with one control UDP payload; copies it.
   peer: PX_PEER_SIZE opaque bytes describing the sender (see plat_set_peer) */
void ctrl_enqueue(const u8 *payload, u32 len, const void *peer);
void ctrl_thread(void *arg);
void ctrl_shutdown_sync(void); /* from other threads: close transfer, flush, set EXIT */
/* called by the ack thread every ack_ms */
void ack_tick(void);
void ack_thread(void *arg);
extern int g_ack_sem;

/* ---------------------------------------------------------------- fsops (fsops.c) */
int fsops_mount(void);
void fsops_update_free(void);
/* request handlers: fill resp body, return status */
int fsops_list(const u8 *req, u32 len, u8 *resp, u32 *rlen);
int fsops_stat(const u8 *req, u32 len, u8 *resp, u32 *rlen);
int fsops_mkdir(const u8 *req, u32 len);
int fsops_delete(const u8 *req, u32 len);
int fsops_rename(const u8 *req, u32 len);
int fsops_df(u8 *resp, u32 *rlen);
int fsops_open_write(const u8 *req, u32 len, u8 *resp, u32 *rlen);
int fsops_close(const u8 *req, u32 len, u8 *resp, u32 *rlen);
int fsops_abort(const u8 *req, u32 len);
int fsops_blocksums(const u8 *req, u32 len, u8 *resp, u32 *rlen);
int fsops_diskinfo(u8 *resp, u32 *rlen);
int fsops_resume_info(const u8 *req, u32 len, u8 *resp, u32 *rlen);
int fsops_read(const u8 *req, u32 len, u32 req_id, u8 *resp, u32 *rlen);
/* writer helpers */
int fsops_write_slot(px_slot_t *s);   /* 0 or PX_E* */
void fsops_set_time(u32 fattime);
void fsops_checkpoint(void);       /* writer: update the resume state if due */

/* ---------------------------------------------------------------- bench */
int bench_run(const u8 *req, u32 len, u8 *resp, u32 *rlen);
int fsops_bench_create(void *fil, u64 size, px_extent_t *e);
void fsops_bench_remove(void *fil);
/* platform parts: fill up to n values, return count */
u32 plat_bench_micro(u32 *vals, u32 n);
/* fs_iomanx mode only; returns PX_* and fills ticks/idle */
/* fs_iomanx mode: OPL-like read check through bdmfs_fatfs; text into out */
int plat_fscheck(char *out, u32 max);
int plat_bench_iomanx(u64 size, u32 chunk, u32 flags, u64 *wt, u32 *wi, u64 *rt, u32 *ri);

#endif
