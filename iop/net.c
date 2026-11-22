/*
 * Minimal network stack on top of the SMAP driver: Ethernet, ARP responder,
 * IPv4, ICMP echo and UDP on one port. No lwIP.
 *
 * Fast path (DATA): the SMAP thread has already read the first 64 bytes of the
 * frame (Ethernet + IPv4 + UDP + our 22-byte header) by PIO; rxeng_data()
 * decides where the payload goes and the payload is DMA'd from the RX FIFO
 * straight into the write buffer.
 *
 * Replies always go to the source MAC of the request: we never send ARP
 * requests ourselves.
 */
#include "irx_imports.h"
#include "px.h"
#include "smap.h"

typedef struct peer {
    u8 mac[6];
    u8 ip[4];   /* network order */
    u16 port;   /* host order */
} peer_t;

static u8 g_mac[6];
static u8 g_ip[4];
static u16 g_port;
static peer_t g_peer;
static int g_tx_sem = -1;
static u16 g_ip_id;

static u8 g_tx[1536] __attribute__((aligned(64)));      /* control / ACK frames */
static u8 g_small[1536] __attribute__((aligned(64)));   /* ARP / ICMP / control RX copy */

/* profiling sums (prof=1) */
static u32 g_prof_dma, g_prof_total, g_prof_n;

static inline u16 be16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static inline void put_be16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }

static u16 ip_csum(const u8 *p, u32 len)
{
    u32 sum = 0;

    while (len > 1) {
        sum += be16(p);
        p += 2;
        len -= 2;
    }
    if (len)
        sum += (u32)p[0] << 8;
    while (sum >> 16)
        sum = (sum & 0xffff) + (sum >> 16);
    return (u16)~sum;
}

void net_init(u32 ip, u16 port)
{
    iop_sema_t s;

    g_ip[0] = (u8)(ip >> 24);
    g_ip[1] = (u8)(ip >> 16);
    g_ip[2] = (u8)(ip >> 8);
    g_ip[3] = (u8)ip;
    g_port  = port;
    SMAPGetMACAddress(g_mac);
    memcpy(g_stats.mac, g_mac, 6);

    s.attr    = SA_THPRI;
    s.option  = 0;
    s.initial = 1;
    s.max     = 1;
    g_tx_sem  = CreateSema(&s);
}

void plat_set_peer(const void *peer)
{
    WaitSema(g_tx_sem);
    memcpy(&g_peer, peer, sizeof(g_peer));
    SignalSema(g_tx_sem);
}

/* builds Ethernet + IPv4 + UDP headers for g_peer into f; returns 42 */
static u32 build_udp_hdr(u8 *f, u32 payload_len)
{
    u32 iplen = 20 + 8 + payload_len;

    memcpy(f, g_peer.mac, 6);
    memcpy(f + 6, g_mac, 6);
    put_be16(f + 12, 0x0800);
    f[14] = 0x45;
    f[15] = 0;
    put_be16(f + 16, (u16)iplen);
    put_be16(f + 18, g_ip_id++);
    put_be16(f + 20, 0x4000); /* DF */
    f[22] = 64;
    f[23] = 17;
    put_be16(f + 24, 0);
    memcpy(f + 26, g_ip, 4);
    memcpy(f + 30, g_peer.ip, 4);
    put_be16(f + 24, ip_csum(f + 14, 20));
    put_be16(f + 34, g_port);
    put_be16(f + 36, g_peer.port);
    put_be16(f + 38, (u16)(8 + payload_len));
    put_be16(f + 40, 0); /* no UDP checksum (allowed in IPv4) */
    return 42;
}

int plat_send(const void *hdr, u32 hdr_len, const void *data, u32 data_len)
{
    u32 n;

    if (42 + hdr_len + data_len > sizeof(g_tx))
        return -1;
    WaitSema(g_tx_sem);
    n = build_udp_hdr(g_tx, hdr_len + data_len);
    memcpy(g_tx + n, hdr, hdr_len);
    if (data != NULL)
        memcpy(g_tx + n + hdr_len, data, data_len);
    smap_transmit(g_tx, (u16)(n + hdr_len + data_len), NULL, 0);
    SignalSema(g_tx_sem);
    return 0;
}

static void send_raw(u8 *frame, u32 len)
{
    WaitSema(g_tx_sem);
    smap_transmit(frame, (u16)len, NULL, 0);
    SignalSema(g_tx_sem);
}

/* ARP request for our IP -> reply (frame in g_small, >= 42 bytes) */
static void handle_arp(u8 *f)
{
    if (be16(f + 14) != 1 || be16(f + 16) != 0x0800 || f[18] != 6 || f[19] != 4 ||
        be16(f + 20) != 1 || memcmp(f + 38, g_ip, 4) != 0)
        return;
    memcpy(f, f + 6, 6);          /* dst = requester */
    memcpy(f + 6, g_mac, 6);
    put_be16(f + 20, 2);          /* reply */
    memcpy(f + 32, f + 22, 10);   /* target = sender (MAC + IP) */
    memcpy(f + 22, g_mac, 6);
    memcpy(f + 28, g_ip, 4);
    send_raw(f, 42);
}

/* ICMP echo request -> reply; the whole frame is in g_small */
static void handle_icmp(u8 *f, u32 len)
{
    u32 iplen = be16(f + 16);

    if (iplen < 28 || 14 + iplen > len || f[34] != 8)
        return;
    memcpy(f, f + 6, 6);
    memcpy(f + 6, g_mac, 6);
    memcpy(f + 30, f + 26, 4);
    memcpy(f + 26, g_ip, 4);
    f[22] = 64;
    put_be16(f + 24, 0);
    put_be16(f + 24, ip_csum(f + 14, 20));
    f[34] = 0; /* echo reply */
    put_be16(f + 36, 0);
    put_be16(f + 36, ip_csum(f + 34, iplen - 20));
    send_raw(f, 14 + iplen);
}

static void rd_payload(void *ctx, void *dst, u32 len)
{
    (void)ctx;
    if (g_cfg.prof) {
        u64 t0 = plat_ticks();
        smap_fifo_read(64, dst, len);
        g_prof_dma += (u32)(plat_ticks() - t0);
    } else {
        smap_fifo_read(64, dst, len);
    }
}

/* SMAP RX callback: runs on the SMAP thread for every good frame */
static int rx_cb(u16 len, const u8 *hdr, u16 hdr_len)
{
    u16 type;
    u32 plen;
    u64 t0 = 0;

    if (hdr_len < 40)
        return 0;
    type = be16(hdr + 12);

    if (type == 0x0800 && hdr_len >= 44 && hdr[14] == 0x45 && (be16(hdr + 20) & 0x3fff) == 0) {
        int to_us = memcmp(hdr + 30, g_ip, 4) == 0;
        int bcast = hdr[30] == 255 && hdr[31] == 255 && hdr[32] == 255 && hdr[33] == 255;

        if (!to_us && !bcast)
            return 0;
        if (hdr[23] == 17 && be16(hdr + 36) == g_port) {
            plen = be16(hdr + 38);
            if (plen < 8 || 34 + plen > len)
                return 0;
            plen -= 8;

            /* ---- fast path */
            if (hdr[42] == PX_T_DATA && hdr_len == 64 && plen >= PX_DATA_HDR) {
                u32 wl = ((u32)*(const u16 *)(hdr + 62) + 3) & ~3u;
                if (PX_DATA_HDR + wl > plen) {
                    g_stats.rx_drops++;
                    return 0;
                }
                if (g_cfg.prof)
                    t0 = plat_ticks();
                if (rxeng_data(hdr + 42, rd_payload, NULL))
                    rxeng_maybe_ack(1);
                if (g_cfg.prof) {
                    g_prof_total += (u32)(plat_ticks() - t0);
                    if (++g_prof_n == 256) {
                        g_stats.prof_dma     = g_prof_dma / 256;
                        g_stats.prof_total   = g_prof_total / 256;
                        g_stats.prof_samples += 256;
                        g_prof_dma = g_prof_total = g_prof_n = 0;
                    }
                }
                return 0;
            }

            /* ---- control: copy the UDP payload (from frame offset 40, so the
               copy stays 4-aligned) and hand it to the control thread */
            if (plen > PX_MBOX_SIZE || 42 + plen > sizeof(g_small))
                return 0;
            {
                peer_t p;
                smap_fifo_read(40, g_small, (2 + plen + 3) & ~3u);
                memcpy(p.mac, hdr + 6, 6);
                memcpy(p.ip, hdr + 26, 4);
                p.port = be16(hdr + 34);
                ctrl_enqueue(g_small + 2, plen, &p);
            }
            return 0;
        }
        if (hdr[23] == 1 && to_us && len <= sizeof(g_small)) {
            smap_fifo_read(0, g_small, (len + 3) & ~3u);
            handle_icmp(g_small, len);
        }
        return 0;
    }

    if (type == 0x0806 && len >= 42) {
        memcpy(g_small, hdr, hdr_len);
        if (hdr_len == 40) /* 42-byte frame: the last 2 bytes of the target IP */
            smap_fifo_read(40, g_small + 40, 2);
        handle_arp(g_small);
    }
    return 0;
}

/* ANNOUNCE broadcast: lets clients see the PS2, and makes PCSX2's "Sockets"
   network mode bind UDP port 7690 on the host (it only binds ports the PS2
   has sent from). Payload: u8 type, u8[3] 0, u32 proto, u8 ip[4], u8 mac[6],
   u16 port (20 bytes). */
void net_announce(void)
{
    u8 body[20];
    peer_t save;
    u32 n;

    memset(body, 0, sizeof(body));
    body[0] = PX_T_ANNOUNCE;
    wr32(body + 4, PX_PROTO_VERSION);
    memcpy(body + 8, g_ip, 4);
    memcpy(body + 12, g_mac, 6);
    wr16(body + 18, g_port);
    WaitSema(g_tx_sem);
    save = g_peer;
    memset(g_peer.mac, 0xff, 6);
    memset(g_peer.ip, 0xff, 4);
    g_peer.port = g_port;
    n = build_udp_hdr(g_tx, sizeof(body));
    memcpy(g_tx + n, body, sizeof(body));
    smap_transmit(g_tx, (u16)(n + sizeof(body)), NULL, 0);
    g_peer = save;
    SignalSema(g_tx_sem);
}

void net_start(void)
{
    smap_register_rx_callback(rx_cb, 64);
}
