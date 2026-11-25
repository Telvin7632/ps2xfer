/*
 * Status block of the IOP module. Returned to the EE by SIF RPC (for the
 * screen) and to the PC by the STATS command. Only u32 fields and char arrays
 * whose sizes are multiples of 4, so the layout is identical on the IOP, the
 * EE and in Python (struct format documented in docs/PROTOCOL.md).
 */
#ifndef PX_STATS_H
#define PX_STATS_H

#define PX_RPC_SID       0x50585846 /* "FXXP" */
#define PX_RPC_GET_STATS 1
#define PX_RPC_EXIT      2
/* EE sends the physical address of a 64-byte aligned px_stats_t; the IOP
   then pushes the status there by SIF DMA every 250 ms from its high-priority
   ack thread (the RPC thread starves while a transfer keeps the IOP busy) */
#define PX_RPC_SET_PUSH  3
/* EE checksum helper for verify (BLOCKSUMS). The EE sends u32[4]: physical
   addresses of two PX_SUM_CHUNK buffers, PX_SUM_CHUNK, 0. The IOP reads a
   chunk from the disk and sends it with SIF command PX_SIFCMD_CHUNK (the
   data travels as the command's extra DMA); the EE sums it and answers with
   PX_SIFCMD_SUMS. User command ids: each side registers its own table. */
#define PX_RPC_SET_SUM   4
#define PX_SUM_CHUNK     (512 * 1024)
#define PX_SIFCMD_CHUNK  0 /* IOP -> EE, handled on the EE */
#define PX_SIFCMD_SUMS   0 /* EE -> IOP, handled on the IOP */
#define PX_SUM_FIRST     1 /* first chunk of a 1 MiB block: reset the sum */
#define PX_SUM_LAST      2 /* last chunk of the block: report the sum */

typedef struct px_sumcmd {      /* 32 bytes, 16-byte multiple */
    unsigned int sif_hdr[4];    /* SifCmdHeader_t, filled by sceSifSendCmd */
    unsigned int seq, buf, len, flags;
} px_sumcmd_t;

typedef struct px_sumdone {
    unsigned int sif_hdr[4];
    unsigned int seq, csum, flags, reserved;
} px_sumdone_t;

#define PX_STATS_VERSION 2

/* state flags */
#define PX_SF_NET_UP     0x0001 /* SMAP initialised, link up */
#define PX_SF_DISK_OK    0x0002 /* ATA device found */
#define PX_SF_FS_OK      0x0004 /* exFAT mounted */
#define PX_SF_SESSION    0x0008 /* a client said HELLO */
#define PX_SF_XFER       0x0010 /* OPEN_WRITE active */
#define PX_SF_EXIT       0x0020 /* EXIT requested (PC or pad); EE should leave */
#define PX_SF_BENCH      0x0040 /* a bench is running */
#define PX_SF_IOMANX     0x0080 /* fs_mode=iomanx (bdmfs_fatfs, our FatFs off) */
#define PX_SF_ERROR      0x0100 /* see last_error */
#define PX_SF_VERIFY     0x0200 /* the PC is verifying a file (BLOCKSUMS); file/size/written = progress */
#define PX_SF_READ       0x0400 /* the PC is reading a file (get / FTP); file/size/written = progress */

typedef struct px_stats {
    unsigned int version;
    unsigned int flags;
    unsigned int uptime_ms;
    unsigned int ip;          /* a.b.c.d = (a<<24)|(b<<16)|(c<<8)|d */
    unsigned char mac[8];     /* 6 used */
    unsigned int port;
    unsigned int link_mode;   /* 8=100FD 4=100HD 2=10FD 1=10HD, |0x40 pause, 0 = no link */
    unsigned int speed_rev;   /* SPEED revision register */
    unsigned int dma_block;   /* RX payload DMA block size in use */
    unsigned int iop_free;    /* QueryTotalFreeMemSize after setup */
    unsigned int iop_maxfree; /* QueryMaxFreeMemSize after setup */
    unsigned int nbuf;        /* write buffers */
    unsigned int bufsize;     /* bytes per buffer */
    unsigned int cpu_load_x10;/* per mille, from the idle thread */
    unsigned int udma_mode;   /* 0xff = unknown */
    unsigned int disk_sectors_lo, disk_sectors_hi;
    unsigned int fs_total_mb, fs_free_mb; /* 0xffffffff = not computed yet */
    unsigned int cluster_size;
    char disk_model[44];
    /* current transfer */
    char file[64];
    unsigned int file_size_lo, file_size_hi;
    unsigned int written_lo, written_hi;  /* base (received + on disk) */
    unsigned int rx_bytes_lo, rx_bytes_hi;/* payload bytes accepted, whole uptime */
    unsigned int rx_pkts;     /* DATA packets accepted */
    unsigned int rx_dups;
    unsigned int rx_drops;    /* out of window / stale handle */
    unsigned int rx_retx;     /* accepted packets flagged as retransmits */
    unsigned int rx_errors;   /* SMAP BD errors (incl. overruns) */
    unsigned int rx_overruns;
    unsigned int csum_errors;
    unsigned int ctl_drops;   /* control mailbox full */
    unsigned int acks_sent;
    unsigned int disk_errors;
    /* profiling (prof=1): average cycles per accepted packet */
    unsigned int prof_hdr, prof_dma, prof_total, prof_samples;
    /* thread time accounting (prof=1), cumulative, in units of 16 ticks;
       wall time from wake-up to sleep, so it includes preemption */
    unsigned int prof_smap16, prof_smap_wakes;
    unsigned int prof_writer16, prof_writer_slots;
    unsigned int prof_ack16, prof_ack_runs;
    unsigned int wr_phase;  /* writer stage (debug): see writer.c; bits 8+: slot states */
    unsigned int wr_lba;    /* low 32 bits of the LBA being written (debug) */
    char last_error[48];
} px_stats_t;

#endif
