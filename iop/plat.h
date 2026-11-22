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
   path; plat_send() always goes to the last peer set here. */
#define PX_PEER_SIZE 12
void plat_set_peer(const void *peer);

/* ---- called every ack_ms from the ack thread (periodic platform work) */
void plat_periodic(void);
/* stops pushing status into EE memory and waits for the last push
   (before the EE starts another program) */
void plat_push_stop(void);
/* writer: run the next ATA command above the RX thread (on=1) or back at
   the writer priority (on=0; lets RX drain the FIFO right away) */
void plat_writer_boost(int on);

/* ---- CPU load: increments of the idle thread (0 if unsupported) */
u32 plat_idle_count(void);

/* ---- payload source handed to rxeng_data(): copies `len` (multiple of 4)
 *      payload bytes of the current DATA packet to dst. */
typedef void (*plat_payload_reader_t)(void *ctx, void *dst, u32 len);

#endif
