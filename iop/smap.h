/*
 * SMAP (PS2 Ethernet) driver interface.
 *
 * smap.c and smap_xfer.c are taken from Neutrino (github.com/rickgaiser/neutrino,
 * iop/smap, commit 1310a9af7c73), which derives from the ps2sdk smap driver.
 * Licence: Academic Free License 3.0 (Neutrino) / 2.0 (ps2sdk); see
 * THIRD_PARTY.md. Changes for ps2xfer are marked "ps2xfer:".
 */
#ifndef PX_SMAP_H
#define PX_SMAP_H

#include <tamtypes.h>
#include <thbase.h>

#define MODNAME "ps2xfer"

#ifdef DEBUG
#include <stdio.h>
#define M_DEBUG(fmt, ...) printf(MODNAME ": " fmt, ##__VA_ARGS__)
#else
#define M_DEBUG(fmt, ...) do { } while (0)
#endif

struct SmapDriverData
{
    volatile u8 *smap_regbase;
    volatile u8 *emac3_regbase;
    unsigned int TxBufferSpaceAvailable;
    unsigned char NumPacketsInTx;
    unsigned char TxBDIndex;
    unsigned char TxDNVBDIndex;
    unsigned char RxBDIndex;
    int Dev9IntrEventFlag;
    int IntrHandlerThreadID;
    unsigned char SmapDriverStarted; // SMAP driver is started.
    unsigned char SmapIsInitialized; // SMAP driver is initialized (software)
    unsigned char NetDevStopFlag;
    unsigned char EnableLinkCheckTimer;
    unsigned char LinkStatus; // Ethernet link is initialized (hardware)
    unsigned char LinkMode;
    iop_sys_clock_t LinkCheckTimer;
    int NetIFID;
};

/* Event flag bits */
#define SMAP_EVENT_START      0x01
#define SMAP_EVENT_INTR       0x04
#define SMAP_EVENT_LINK_CHECK 0x10

extern struct SmapDriverData SmapDriverData;
extern u16 g_dma_max_block_size;

/* ps2xfer: parameters instead of module arguments */
int smap_init(unsigned int thread_priority, int enable_pause);
int SMAPGetMACAddress(u8 *buffer);

int smap_transmit(void *header, u16 headersize, const void *data, u16 datasize);
/* cb runs on the SMAP thread for every good frame. hdr holds bytes 12 up to
   hdr_len (<= 64) of the frame; bytes 0..11 (MAC addresses) are NOT filled
   (read them with smap_fifo_read(0, hdr, 12) when needed; hdr is writable).
   Return 1 if the frame was read exactly to its end, else 0: the driver then
   moves the read pointer to the end of the frame. */
int smap_register_rx_callback(int (*cb)(u16 len, const u8 *hdr, u16 hdr_len), u16 n_bytes);
/* read bytes of the current RX frame (frame-relative offset); only inside the callback */
void smap_fifo_read(u16 offset, void *dst, u32 bytes);
/* same, DMA only with 128-byte blocks: bytes must be a multiple of 128 */
void smap_fifo_read_dma128(u16 offset, void *dst, u32 bytes);

void xfer_init(void);
int HandleRxIntr(struct SmapDriverData *SmapDrivPrivData);

#endif
