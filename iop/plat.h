/*
 * Platform layer used by the portable core (rxeng, writer, ctrl, fsops, bench).
 * Implemented by plat_ps2.c (IOP) and tests/host/plat_host.c (Linux simulator).
 */
#ifndef PX_PLAT_H
#define PX_PLAT_H

#ifdef _IOP
#include <tamtypes.h>
#else
#include <stdint.h>
#include <stddef.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t s32;
typedef int64_t s64;
#endif

/* ---- short critical section (IOP: interrupts off; host: global mutex) */
int plat_lock(void);
void plat_unlock(int state);

/* ---- counting semaphores */
int plat_sem_create(int initial, int max);
void plat_sem_wait(int sem);
void plat_sem_signal(int sem);

/* ---- time */
u64 plat_ticks(void);          /* monotonic */
u32 plat_ticks_per_ms(void);
void plat_sleep_us(u32 usec);

/* ---- memory */
void *plat_alloc(u32 size);   /* 64-byte aligned, NULL on failure */
void plat_free(void *p);
u32 plat_max_free_mem(void);
u32 plat_total_free_mem(void);

/* ---- whole-disk block device (512-byte sectors), serialised by the caller
 *      through px_ata_lock()/px_ata_unlock() */
int plat_bd_read(u64 lba, void *buf, u32 count);         /* 0 or -1 */
int plat_bd_write(u64 lba, const void *buf, u32 count);  /* 0 or -1 */
void plat_bd_flush(void);
u64 plat_bd_sectors(void);                               /* 0 = no disk */

/* ---- network: send one UDP payload to the current peer.
 *      `data` may be NULL; hdr and data are concatenated. */
int plat_send(const void *hdr, u32 hdr_len, const void *data, u32 data_len);
/* peer = sender of a control request (MAC, IP, UDP port), recorded by the RX
   path. plat_send() answers the last request's sender; ACK and READ_DATA go
   to the data peer, which only the session owner can set (a stray packet
   from another host must not redirect the data stream). */
#define PX_PEER_SIZE 12
void plat_set_data_peer(void);   /* data peer = current request's sender */
int plat_send_ack(const void *buf, u32 len);
/* bulk send to the current peer: 22-byte protocol header + 4-aligned data
   (IOP: header and data go to the TX FIFO by DMA, data is not copied) */
int plat_send_data(const void *hdr22, const void *data, u32 data_len);
void plat_set_peer(const void *peer);

/* ---- called every ack_ms from the ack thread (periodic platform work) */
void plat_periodic(void);
/* stops pushing status into EE memory and waits for the last push
   (before the EE starts another program) */
void plat_push_stop(void);

/* EE checksum helper for verify (PS2 only; the host has none).
 * plat_ee_sum_send: SIF DMA id, 0 = could not queue. len <= PX_SUM_CHUNK;
 * src must have room up to len rounded to 16 (the tail is zeroed).
 * plat_ee_sum_result: 1 when the answer for seq arrived. */
int plat_ee_sum_ready(void);
u32 plat_ee_sum_send(u32 seq, int buf, void *src, u32 len, u32 flags);
int plat_ee_dma_done(u32 trid);
int plat_ee_sum_result(u32 seq, u32 *csum, u32 *flags);
void plat_ee_sum_disable(void);
/* writer: run the next ATA command above the RX thread (on=1) or back at
   the writer priority (on=0; lets RX drain the FIFO right away) */
void plat_writer_boost(int on);

/* ---- CPU load: increments of the idle thread (0 if unsupported) */
u32 plat_idle_count(void);

/* ---- payload source handed to rxeng_data(): copies `len` (multiple of 4)
 *      payload bytes of the current DATA packet to dst. */
typedef void (*plat_payload_reader_t)(void *ctx, void *dst, u32 len);

#endif
