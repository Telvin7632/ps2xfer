/*
 * Control thread: takes control requests from the mailbox (filled by the RX
 * path), runs them and sends responses. Retries of a request (same req_id)
 * get the cached response instead of running the command again.
 */
#ifdef _IOP
#include "irx_imports.h"
#else
#include <string.h>
#endif
#include "px.h"

typedef struct mbox_slot {
    u32 len;
    u8 peer[PX_PEER_SIZE];
    u8 data[PX_MBOX_SIZE] __attribute__((aligned(4)));
} mbox_slot_t;

static mbox_slot_t g_mbox[PX_MBOX_SLOTS];
static u32 g_mbox_head, g_mbox_tail; /* head: producer (RX), tail: consumer */
static int g_mbox_sem = -1;

#define CACHE_N 4
typedef struct resp_cache {
    u32 req_id;
    u32 len;
    u8 data[PX_MBOX_SIZE];
} resp_cache_t;
static resp_cache_t g_cache[CACHE_N];
static u32 g_cache_next;

static volatile int g_shutdown_req;
static int g_shutdown_done = -1;
/* Several clients may be connected at once (a send, the GUI, the FTP
 * gateway, a stats poll): each HELLO adds a session, the oldest drops out.
 * The data peer (ACKs, READ_DATA) is taken only by OPEN_WRITE and READ. */
#define NSESS 8
static struct { u32 id, last_ms; } g_sess[NSESS];
static u32 g_session;      /* the newest one (HELLO reply) */
static u32 g_xfer_session; /* the one that opened the running write */
static u8 g_resp[PX_MBOX_SIZE] __attribute__((aligned(4)));
static u32 g_fattime;

int ctrl_init(void)
{
    g_mbox_sem = plat_sem_create(0, PX_MBOX_SLOTS + 4);
    g_shutdown_done = plat_sem_create(0, 4);
    return (g_mbox_sem < 0 || g_shutdown_done < 0) ? -1 : 0;
}

void ctrl_enqueue(const u8 *payload, u32 len, const void *peer)
{
    mbox_slot_t *m;
    int st;

    if (len < PX_CTL_HDR || len > PX_MBOX_SIZE) {
        g_stats.ctl_drops++;
        return;
    }
    st = plat_lock();
    if (g_mbox_head - g_mbox_tail >= PX_MBOX_SLOTS) {
        plat_unlock(st);
        g_stats.ctl_drops++;
        return;
    }
    m = &g_mbox[g_mbox_head % PX_MBOX_SLOTS];
    plat_unlock(st);

    memcpy(m->data, payload, len);
    memcpy(m->peer, peer, PX_PEER_SIZE);
    m->len = len;

    st = plat_lock();
    g_mbox_head++;
    plat_unlock(st);
    plat_sem_signal(g_mbox_sem);
}

static void send_resp(const u8 *req, int status, u32 body_len)
{
    u32 len = PX_CTL_HDR + body_len;
    resp_cache_t *c;

    g_resp[0] = req[0];
    g_resp[1] = PX_F_RESPONSE;
    wr16(g_resp + 2, (u16)status);
    memcpy(g_resp + 4, req + 4, 4);   /* req_id */
    if (req[0] == PX_T_HELLO)
        wr32(g_resp + 8, g_session);
    else
        memcpy(g_resp + 8, req + 8, 4); /* the asking client's session */
    plat_send(g_resp, len, NULL, 0);

    /* read-only answers are not cached: a monitor polling stats must not
     * evict the replies the transferring client may still need */
    if (req[0] == PX_T_PING || req[0] == PX_T_STATS || req[0] == PX_T_LOG || req[0] == PX_T_HELLO)
        return;
    c = &g_cache[g_cache_next++ % CACHE_N];
    c->req_id = rd32(req + 4);
    c->len    = len;
    memcpy(c->data, g_resp, len);
}

static int cached_resend(const u8 *req)
{
    u32 id = rd32(req + 4), i;

    for (i = 0; i < CACHE_N; i++) {
        if (g_cache[i].len != 0 && g_cache[i].req_id == id && g_cache[i].data[0] == req[0] &&
            rd32(g_cache[i].data + 8) == rd32(req + 8)) {
            plat_send(g_cache[i].data, g_cache[i].len, NULL, 0);
            return 1;
        }
    }
    return 0;
}

/* EXIT (PC, pad or power-off): keep an open transfer resumable, flush the
   disk and give the write buffers back, so the EE can load the memory card
   driver for exit_elf (the IOP is otherwise full). Runs on the control thread. */
static void do_shutdown(void)
{
    int st;

    plat_push_stop(); /* the EE may start another program soon */
    if (g_rx.active)
        fsops_abort(NULL, 0);
    px_ata_lock();
    plat_bd_flush();
    px_ata_unlock();
    st = plat_lock();
    g_rx.active = 0;
    plat_unlock(st);
    if (g_bufmem != NULL) {
        plat_free(g_bufmem);
        g_bufmem      = NULL;
        g_bufmem_size = 0;
    }
    g_stats.flags |= PX_SF_EXIT;
}

static u32 do_hello(const u8 *body, u32 blen, u8 *out)
{
    u64 disk = plat_bd_sectors() * 512;

    if (blen >= 12) {
        g_fattime = rd32(body + 8);
        fsops_set_time(g_fattime);
    }
    g_session = (u32)plat_ticks() ^ (blen >= 8 ? rd32(body + 4) : 0) ^ 0x5A5A1234u;
    if (g_session == 0)
        g_session = 1;
    {
        /* replace the least recently used session, never the running write's */
        u32 i, best = NSESS, now = px_ms();
        for (i = 0; i < NSESS; i++) {
            if (g_sess[i].id == 0) {
                best = i;
                break;
            }
            if (g_rx.active && g_sess[i].id == g_xfer_session)
                continue;
            if (best == NSESS || now - g_sess[i].last_ms > now - g_sess[best].last_ms)
                best = i;
        }
        g_sess[best].id      = g_session;
        g_sess[best].last_ms = now;
    }
    g_stats.flags |= PX_SF_SESSION;

    memset(out, 0, 136);
    wr32(out + 0, PX_PROTO_VERSION);
    wr32(out + 4, g_session);
    wr32(out + 8, g_cfg.ip);
    memcpy(out + 12, g_stats.mac, 6);
    wr16(out + 18, (u16)g_cfg.port);
    wr32(out + 20, PX_PAYLOAD);
    wr32(out + 24, PX_UNIT);
    wr32(out + 28, g_rx.S);
    wr32(out + 32, g_rx.B);
    wr32(out + 36, g_stats.flags);
    wr32(out + 40, g_stats.link_mode);
    wr32(out + 44, g_stats.udma_mode);
    wr32(out + 48, g_stats.speed_rev);
    wr32(out + 52, g_stats.iop_free);
    wr64(out + 56, disk);
    wr32(out + 64, g_stats.cluster_size);
    wr32(out + 68, g_stats.fs_total_mb);
    wr32(out + 72, g_stats.fs_free_mb);
    memcpy(out + 76, g_stats.disk_model, 43);
    strncpy((char *)out + 120, PX_VERSION_STR, 15);
    px_log("HELLO: session %08x\n", (unsigned)g_session);
    return 136;
}

static int do_set(const u8 *body, u32 blen)
{
    char kv[64];
    u32 n = blen < sizeof(kv) - 1 ? blen : sizeof(kv) - 1;
    u32 old_bufs = g_cfg.bufs, old_units = g_cfg.buf_units;

    memcpy(kv, body, n);
    kv[n] = '\0';
    /* buffer geometry may change only while no transfer is active */
    if (px_cfg_set(&g_cfg, kv, g_rx.active ? 1 : 0) != 0 ||
        (g_rx.active && (strncmp(kv, "bufs=", 5) == 0 || strncmp(kv, "buf_units=", 10) == 0)))
        return PX_EINVAL;
    if (g_cfg.bufs != old_bufs || g_cfg.buf_units != old_units) {
        if (rxeng_alloc() != 0) {
            g_cfg.bufs = old_bufs;
            g_cfg.buf_units = old_units;
            rxeng_alloc();
            return PX_ENOSPC;
        }
    }
    px_log("SET %s\n", kv);
    return PX_OK;
}

static void dispatch(const u8 *req, u32 len)
{
    u8 type = req[0];
    const u8 *body = req + PX_CTL_HDR;
    u32 blen = len - PX_CTL_HDR;
    u8 *out = g_resp + PX_CTL_HDR;
    u32 olen = 0;
    int status = PX_OK;

    if ((req[1] & PX_F_RESPONSE) || type == PX_T_ANNOUNCE)
        return; /* not for us: answers and other consoles' announcements */
    /* PING, STATS and LOG only read; anyone may ask without a session, and
     * they never move the data peer, so a monitor cannot disturb a transfer */
    int observe = type == PX_T_PING || type == PX_T_STATS || type == PX_T_LOG;
    u32 sess = rd32(req + 8), i;

    if (type != PX_T_HELLO && !observe) {
        for (i = 0; i < NSESS && (sess == 0 || g_sess[i].id != sess); i++)
            ;
        if (i == NSESS) {
            send_resp(req, PX_ESESSION, 0);
            return;
        }
        g_sess[i].last_ms = px_ms();
        if (cached_resend(req))
            return;
    }

    /* OPEN_WRITE and RESUME_INFO close an open transfer. Another client may
       not do that while the file is still streaming in; a silent one
       (crashed, cable) may be replaced so that its file can be resumed. */
    if ((type == PX_T_OPEN_WRITE || type == PX_T_RESUME_INFO) && rxeng_busy() && sess != g_xfer_session) {
        px_log("%s refused: another client is sending\n", type == PX_T_OPEN_WRITE ? "OPEN_WRITE" : "RESUME_INFO");
        send_resp(req, PX_EBUSY, 0);
        return;
    }

    switch (type) {
        case PX_T_HELLO:
            olen = do_hello(body, blen, out);
            break;
        case PX_T_PING:
            wr32(out, px_ms());
            olen = 4;
            break;
        case PX_T_SET:
            status = do_set(body, blen);
            break;
        case PX_T_STATS:
            memcpy(out, &g_stats, sizeof(g_stats));
            olen = sizeof(g_stats);
            break;
        case PX_T_LOG: {
            u32 cursor = blen >= 4 ? rd32(body) : 0, lost;
            u32 n = px_log_read(&cursor, (char *)out + 8, PX_MBOX_SIZE - PX_CTL_HDR - 8 - 64, &lost);
            wr32(out, cursor);
            wr32(out + 4, lost);
            olen = 8 + n;
            break;
        }
        case PX_T_EXIT:
            px_log("EXIT requested by the PC\n");
            do_shutdown();
            break;
        case PX_T_LIST:
            status = fsops_list(body, blen, out, &olen);
            break;
        case PX_T_STAT:
            status = fsops_stat(body, blen, out, &olen);
            break;
        case PX_T_MKDIR:
            status = fsops_mkdir(body, blen);
            break;
        case PX_T_DELETE:
            status = fsops_delete(body, blen);
            break;
        case PX_T_RENAME:
            status = fsops_rename(body, blen);
            break;
        case PX_T_DF:
            status = fsops_df(out, &olen);
            break;
        case PX_T_OPEN_WRITE:
            plat_set_data_peer(); /* ACKs go to this client */
            g_xfer_session = sess;
            status = fsops_open_write(body, blen, out, &olen);
            break;
        case PX_T_CLOSE:
            status = fsops_close(body, blen, out, &olen);
            break;
        case PX_T_ABORT:
            status = fsops_abort(body, blen);
            break;
        case PX_T_BLOCKSUMS:
            status = fsops_blocksums(body, blen, out, &olen);
            break;
        case PX_T_RESUME_INFO:
            status = fsops_resume_info(body, blen, out, &olen);
            break;
        case PX_T_DISKINFO:
            status = fsops_diskinfo(out, &olen);
            break;
        case PX_T_READ:
            if (!g_rx.active)
                plat_set_data_peer(); /* READ_DATA goes to this client */
            status = fsops_read(body, blen, rd32(req + 4), out, &olen);
            break;
        case PX_T_BENCH:
            g_stats.flags |= PX_SF_BENCH;
            status = bench_run(body, blen, out, &olen);
            g_stats.flags &= ~PX_SF_BENCH;
            break;
        default:
            status = PX_ENOTSUP;
            break;
    }
    if (status != PX_OK && type != PX_T_CLOSE && type != PX_T_BENCH)
        olen = 0;
    send_resp(req, status, olen);
}

/* Called from other threads (power-off callback, EE exit request): the
   control thread closes an open transfer resumably and flushes the disk. */
void ctrl_shutdown_sync(void)
{
    g_shutdown_req = 1;
    plat_sem_signal(g_mbox_sem);
    plat_sem_wait(g_shutdown_done);
}

void ctrl_thread(void *arg)
{
    (void)arg;

    for (;;) {
        mbox_slot_t *m;

        plat_sem_wait(g_mbox_sem);
        if (g_shutdown_req) {
            g_shutdown_req = 0;
            do_shutdown();
            plat_sem_signal(g_shutdown_done);
            continue;
        }
        m = &g_mbox[g_mbox_tail % PX_MBOX_SLOTS];
        plat_set_peer(m->peer);
        dispatch(m->data, m->len);
        {
            int st = plat_lock();
            g_mbox_tail++;
            plat_unlock(st);
        }
    }
}
