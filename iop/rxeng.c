/*
 * Receive engine: maps DATA packets into the write buffers ("slots"), keeps
 * the received bitmaps, the window/credit and builds ACKs.
 *
 * Slots cover consecutive file ranges [W + i*S, W + (i+1)*S), i = 0..B-1.
 * The slot of W has index W_idx; the writer advances W when the oldest slot
 * has been written. credit = W + B*S, i.e. the PC only ever gets credit for
 * buffer memory that exists and is free.
 *
 * Threads (IOP): rxeng_data() runs on the SMAP thread (highest priority of
 * ours); the writer and the ack thread are lower priority and change shared
 * state only inside plat_lock() (interrupts off), so on the IOP the RX path
 * needs no locking. On the host simulator threads run in parallel, so the RX
 * path takes the lock too (RX_LOCK).
 */
#ifdef _IOP
#include "irx_imports.h"
#define RX_LOCK()     0
#define RX_UNLOCK(st) (void)(st)
#else
#include <string.h>
#define RX_LOCK()     plat_lock()
#define RX_UNLOCK(st) plat_unlock(st)
#endif
#include "px.h"

px_rx_t g_rx;
static u32 g_csum_ctr;
u8 *g_bufmem;
u32 g_bufmem_size;

int rxeng_alloc(void)
{
    u32 S, B, i, avail;

    if (g_bufmem != NULL) {
        plat_free(g_bufmem);
        g_bufmem = NULL;
    }

    S = g_cfg.buf_units * PX_UNIT;
    B = g_cfg.bufs;
    avail = plat_max_free_mem();
    /* keep 128 KiB for everything else */
    while (B >= 2 && (u64)B * S + 128 * 1024 > avail)
        B--;
    while (B >= 2 && (g_bufmem = plat_alloc(B * S)) == NULL)
        B--;
    if (g_bufmem == NULL) {
        px_set_error("no memory for 2 write buffers");
        return -1;
    }

    g_bufmem_size = B * S;
    memset(&g_rx, 0, sizeof(g_rx));
    g_rx.S = S;
    g_rx.B = B;
    g_rx.P = S / PX_PAYLOAD;
    for (i = 0; i < B; i++)
        g_rx.slot[i].mem = g_bufmem + i * S;

    g_stats.nbuf    = B;
    g_stats.bufsize = S;
    px_log("buffers: %u x %u bytes (max free %u)\n", (unsigned)B, (unsigned)S, (unsigned)avail);
    return 0;
}

static void slot_init(px_slot_t *s, u64 base)
{
    u32 valid;

    s->base = base;
    s->rcvd = 0;
    memset(s->bitmap, 0, sizeof(s->bitmap));
    if (base >= g_rx.size) {
        s->valid = 0;
        s->npkts = 0;
        s->state = SLOT_IDLE;
        return;
    }
    valid    = (g_rx.size - base < g_rx.S) ? (u32)(g_rx.size - base) : g_rx.S;
    s->valid = valid;
    s->npkts = (u16)((valid + PX_PAYLOAD - 1) / PX_PAYLOAD);
    s->state = SLOT_FILLING;
}

void rxeng_start(u16 handle, u64 size, u64 start, u32 flags)
{
    u32 i;
    int st = plat_lock();

    g_rx.active         = 0;
    g_rx.handle         = handle;
    g_rx.flags          = flags;
    g_rx.size           = size;
    g_rx.W              = start;
    g_rx.W_idx          = 0;
    g_rx.W_pkt          = (u32)(start / PX_PAYLOAD);
    g_rx.durable        = start;
    g_rx.max_pkt_seq    = 0;
    g_rx.pkts_since_ack = 0;
    g_rx.last_ack_ms    = px_ms();
    g_rx.last_data_ms   = g_rx.last_ack_ms;
    g_rx.seen_pkts      = g_stats.rx_pkts;
    g_rx.credit_changed = 1;
    g_rx.manifest       = 0;
    g_rx.error          = 0;
    for (i = 0; i < g_rx.B; i++)
        slot_init(&g_rx.slot[i], start + (u64)i * g_rx.S);
    g_rx.active = 1;
    plat_unlock(st);

    g_stats.file_size_lo = (u32)size;
    g_stats.file_size_hi = (u32)(size >> 32);
    g_stats.written_lo   = (u32)start;
    g_stats.written_hi   = (u32)(start >> 32);
    g_stats.flags |= PX_SF_XFER;
}

void rxeng_stop(void)
{
    int st = plat_lock();

    g_rx.active = 0;
    plat_unlock(st);
    g_stats.flags &= ~PX_SF_XFER;
}

/*
 * hdr: the 22-byte DATA header; hdr + 2 must be 4-byte aligned (it is in the
 * SMAP header buffer: frame offset 44).
 */
/* Data arrived recently: another client must not take over the session. */
int rxeng_busy(void)
{
    return g_rx.active && px_ms() - g_rx.last_data_ms < 2000;
}

int rxeng_data(const u8 *hdr, plat_payload_reader_t rd, void *ctx)
{
    u8 flags     = hdr[1];
    u64 offset   = (u64) * (const u32 *)(hdr + 2) | ((u64) * (const u32 *)(hdr + 6) << 32);
    u32 pkt_seq  = *(const u32 *)(hdr + 10);
    u32 csum     = *(const u32 *)(hdr + 14);
    u16 handle   = *(const u16 *)(hdr + 18);
    u16 len      = *(const u16 *)(hdr + 20);
    px_slot_t *s;
    u64 rel64;
    u32 rel, si, inner, pi, explen, wl, pktno, bit;
    u8 *dst;
    int st;

    st = RX_LOCK();
    if (!g_rx.active || handle != g_rx.handle || g_rx.error) {
        RX_UNLOCK(st);
        g_stats.rx_drops++;
        return 0;
    }
    if ((s32)(pkt_seq - g_rx.max_pkt_seq) > 0)
        g_rx.max_pkt_seq = pkt_seq;

    if (offset < g_rx.W) { /* already on disk: our ACK was lost */
        g_rx.pkts_since_ack++;
        RX_UNLOCK(st);
        g_stats.rx_dups++;
        return g_rx.pkts_since_ack >= g_cfg.ack_every;
    }
    rel64 = offset - g_rx.W;
    if (rel64 >= (u64)g_rx.B * g_rx.S) {
        RX_UNLOCK(st);
        g_stats.rx_drops++;
        return 0;
    }
    /* no divisions on the hot path (divu costs ~35 IOP cycles each):
       slot by subtraction (< B iterations); packet index = (inner / 128) / 11
       by multiplication, exact for inner / 128 <= 5632, i.e. any slot up to
       PX_MAX_BUF_UNITS * PX_UNIT (checked exhaustively) */
    rel   = (u32)rel64;
    si    = 0;
    inner = rel;
    while (inner >= g_rx.S) {
        inner -= g_rx.S;
        si++;
    }
    pi = ((inner >> 7) * 0xBA2Fu) >> 19;
    {
        u32 idx = g_rx.W_idx + si;
        if (idx >= g_rx.B)
            idx -= g_rx.B;
        s = &g_rx.slot[idx];
    }
    bit   = 1u << (pi & 31);

    if (inner != pi * PX_PAYLOAD || s->state != SLOT_FILLING || pi >= s->npkts) {
        RX_UNLOCK(st);
        if (s->state != SLOT_FILLING)
            g_stats.rx_dups++;
        else
            g_stats.rx_drops++;
        return 0;
    }
    if (s->bitmap[pi >> 5] & bit) {
        g_rx.pkts_since_ack++;
        RX_UNLOCK(st);
        g_stats.rx_dups++;
        return g_rx.pkts_since_ack >= g_cfg.ack_every;
    }
    explen = (pi + 1 == s->npkts) ? s->valid - pi * PX_PAYLOAD : PX_PAYLOAD;
    /* before the payload read: while the RX thread waits for the DEV9 DMA
       lock the writer may advance W/W_pkt */
    pktno = g_rx.W_pkt + si * g_rx.P + pi;
    RX_UNLOCK(st);

    if (len != explen) {
        g_stats.rx_drops++;
        return 0;
    }

    /* the payload goes straight into the write buffer */
    wl  = ((u32)len + 3) & ~3u;
    dst = s->mem + inner;
    if (!(g_rx.flags & PX_OW_SINK_NODMA)) {
        rd(ctx, dst, wl);
        if (g_cfg.csum != 0 && ++g_csum_ctr >= g_cfg.csum) { /* every N-th packet */
            g_csum_ctr = 0;
            if (px_fletcher(dst, wl) != csum) {
                g_stats.csum_errors++;
                return 1; /* bit stays clear: SACK makes the PC resend it */
            }
        }
    }

    st = RX_LOCK();
    if (s->state != SLOT_FILLING || s->base + inner != offset || (s->bitmap[pi >> 5] & bit)) {
        RX_UNLOCK(st); /* host only: lost a race with a duplicate */
        return 0;
    }
    s->bitmap[pi >> 5] |= bit;
    s->rcvd++;
    g_rx.manifest += (u32)(csum + pktno * PX_MANIFEST_MUL);
    g_rx.pkts_since_ack++;
    if (s->rcvd == s->npkts) {
        s->state = SLOT_FULL;
        RX_UNLOCK(st);
        plat_sem_signal(g_writer_sem);
        st = RX_LOCK();
    }
    RX_UNLOCK(st);

    g_stats.rx_pkts++;
    if (flags & PX_DF_RETX)
        g_stats.rx_retx++;
    {
        u64 b = ((u64)g_stats.rx_bytes_hi << 32 | g_stats.rx_bytes_lo) + len;
        g_stats.rx_bytes_lo = (u32)b;
        g_stats.rx_bytes_hi = (u32)(b >> 32);
    }
    return g_rx.pkts_since_ack >= g_cfg.ack_every;
}

u32 rxeng_build_ack(u8 *buf)
{
    u32 i, j, P, bits, maxbits, nbytes;
    u64 credit;
    int st = plat_lock();

    P = g_rx.S / PX_PAYLOAD; /* packets per slot (multiple of 32) */
    buf[0] = PX_T_ACK;
    buf[1] = 0;
    wr16(buf + 2, g_rx.handle);
    wr32(buf + 4, g_rx.max_pkt_seq);
    wr64(buf + 8, g_rx.W);
    credit = g_rx.W + (u64)g_rx.B * g_rx.S;
    if (credit > g_rx.size)
        credit = g_rx.size;
    wr64(buf + 16, credit);
    wr64(buf + 24, g_rx.durable);
    wr32(buf + 32, g_stats.rx_errors);
    wr32(buf + 36, g_stats.rx_drops);
    wr32(buf + 40, g_stats.csum_errors);
    if (g_rx.W >= g_rx.size)
        buf[1] |= PX_AF_COMPLETE;
    if (g_rx.error)
        buf[1] |= PX_AF_ERROR;
    wr16(buf + 46, (u16)g_rx.error);

    maxbits = (u32)((g_rx.size - g_rx.W + PX_PAYLOAD - 1) / PX_PAYLOAD);
    bits    = g_rx.B * P;
    if (bits > maxbits)
        bits = maxbits;
    wr16(buf + 44, (u16)bits);

    nbytes = (bits + 7) / 8;
    memset(buf + PX_ACK_HDR, 0, (nbytes + 3) & ~3u);
    for (i = 0; i < g_rx.B && i * P < bits; i++) {
        const px_slot_t *s = &g_rx.slot[(g_rx.W_idx + i) % g_rx.B];
        u8 *out = buf + PX_ACK_HDR + i * (P / 8);

        if (s->state == SLOT_FILLING) {
            for (j = 0; j < P / 32; j++)
                wr32(out + j * 4, s->bitmap[j]);
        } else if (s->state != SLOT_IDLE) {
            memset(out, 0xff, s->npkts >> 3);
            if (s->npkts & 7)
                out[s->npkts >> 3] = (u8)((1u << (s->npkts & 7)) - 1);
        }
    }

    g_rx.pkts_since_ack = 0;
    g_rx.credit_changed = 0;
    g_rx.last_ack_ms    = px_ms();
    plat_unlock(st);
    return PX_ACK_HDR + nbytes;
}

void rxeng_maybe_ack(int force)
{
    u8 buf[PX_ACK_HDR + PX_MAX_BUFS * PX_MAX_SLOT_PKTS / 8 + 4] __attribute__((aligned(4)));
    u32 len, now;

    if (!g_rx.active)
        return;
    now = px_ms();
    if (g_stats.rx_pkts != g_rx.seen_pkts) {
        g_rx.seen_pkts    = g_stats.rx_pkts;
        g_rx.last_data_ms = now;
    }
    if (!force) {
        if (!(g_rx.credit_changed ||
              (g_rx.pkts_since_ack > 0 && now - g_rx.last_ack_ms >= g_cfg.ack_ms) ||
              now - g_rx.last_ack_ms >= 20))
            return;
    }
    len = rxeng_build_ack(buf);
    plat_send_ack(buf, len);
    g_stats.acks_sent++;
}
