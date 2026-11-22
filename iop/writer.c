/*
 * Writer thread: writes FULL slots to disk (or discards them in sink mode),
 * then advances the window. Ack thread: periodic / on-demand ACKs.
 */
#ifdef _IOP
#include "irx_imports.h"
#else
#include <string.h>
#endif
#include "px.h"

int g_writer_sem = -1;
int g_ack_sem    = -1;

/* lowest-base FULL slot, marked WRITING; NULL if none */
static px_slot_t *take_full_slot(void)
{
    px_slot_t *best = NULL;
    u32 i;
    int st = plat_lock();

    if (g_rx.active && !g_rx.error) {
        for (i = 0; i < g_rx.B; i++) {
            px_slot_t *s = &g_rx.slot[i];
            if (s->state == SLOT_FULL && (best == NULL || s->base < best->base))
                best = s;
        }
        if (best != NULL)
            best->state = SLOT_WRITING;
    }
    plat_unlock(st);
    return best;
}

static void finish_slot(px_slot_t *s, int err)
{
    int st = plat_lock();

    if (err) {
        g_rx.error = err;
        s->state   = SLOT_FULL; /* keep the data; session is stopped */
        plat_unlock(st);
        g_stats.disk_errors++;
        px_set_error("disk write failed");
        return;
    }
    s->state = SLOT_DONE;
    /* advance W over consecutive written slots and reuse them */
    for (;;) {
        px_slot_t *w = &g_rx.slot[g_rx.W_idx];
        u64 next;

        if (w->state != SLOT_DONE)
            break;
        next = w->base + (u64)g_rx.B * g_rx.S;
        g_rx.W = w->base + g_rx.S;
        if (g_rx.W > g_rx.size)
            g_rx.W = g_rx.size;
        g_rx.W_pkt += g_rx.P;
        g_rx.W_idx = (g_rx.W_idx + 1) % g_rx.B;
        /* slot_init() inlined: rxeng.c owns the layout */
        w->base = next;
        w->rcvd = 0;
        memset(w->bitmap, 0, sizeof(w->bitmap));
        if (next >= g_rx.size) {
            w->valid = 0;
            w->npkts = 0;
            w->state = SLOT_IDLE;
        } else {
            w->valid = (g_rx.size - next < g_rx.S) ? (u32)(g_rx.size - next) : g_rx.S;
            w->npkts = (u16)((w->valid + PX_PAYLOAD - 1) / PX_PAYLOAD);
            w->state = SLOT_FILLING;
        }
        g_rx.credit_changed = 1;
    }
    g_stats.written_lo = (u32)g_rx.W;
    g_stats.written_hi = (u32)(g_rx.W >> 32);
    plat_unlock(st);
}

void writer_thread(void *arg)
{
    (void)arg;

    for (;;) {
        px_slot_t *s;

        g_stats.wr_phase = 1; /* waiting for a full slot */
        plat_sem_wait(g_writer_sem);
        while ((s = take_full_slot()) != NULL) {
            int err = 0;

            g_stats.wr_phase = 2; /* writing a slot */

            u64 t0 = g_cfg.prof ? plat_ticks() : 0;

            if (!(g_rx.flags & (PX_OW_SINK | PX_OW_SINK_NODMA)))
                err = fsops_write_slot(s);
            if (t0) {
                g_stats.prof_writer16 += (u32)((plat_ticks() - t0) >> 4);
                g_stats.prof_writer_slots++;
            }
            g_stats.wr_phase = 3; /* finishing */
            finish_slot(s, err);
            plat_sem_signal(g_ack_sem); /* credit changed: ACK now */
            g_stats.wr_phase = 4; /* checkpoint */
            if (!err)
                fsops_checkpoint();
            g_stats.wr_phase = 5;
        }
    }
}

void ack_tick(void)
{
    u32 i, ss = 0;

    for (i = 0; i < g_rx.B && i < 6; i++)
        ss |= (u32)g_rx.slot[i].state << (8 + 4 * i);
    g_stats.wr_phase = (g_stats.wr_phase & 0xff) | ss;
    rxeng_maybe_ack(0);
    fsops_task_expire();
    px_load_update();
    plat_periodic();
}

void ack_thread(void *arg)
{
    (void)arg;
    for (;;) {
        u64 t0;

        plat_sem_wait(g_ack_sem);
        t0 = g_cfg.prof ? plat_ticks() : 0;
        ack_tick();
        if (t0) {
            g_stats.prof_ack16 += (u32)((plat_ticks() - t0) >> 4);
            g_stats.prof_ack_runs++;
        }
    }
}
