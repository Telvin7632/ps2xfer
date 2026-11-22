/*
 * Status screen drawn with gsKit and the console's built-in FONTM font.
 * Everything stays inside a title-safe area (8 % margins) and the whole
 * picture can be moved with screen_x/screen_y (ps2xfer.ini) or the D-pad.
 */
#include <stdio.h>
#include <string.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <gsToolkit.h>
#include <gsFontM.h>

#include "px_stats.h"
#include "ui.h"

static GSGLOBAL *gs;
static GSFONTM *font;
static int g_off_x, g_off_y;

#define RGB(r, g, b) GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00)
#define C_BG     RGB(0x10, 0x14, 0x20)
#define C_PANEL  RGB(0x1c, 0x24, 0x38)
#define C_TEXT   RGB(0xe8, 0xe8, 0xf0)
#define C_DIM    RGB(0x90, 0x98, 0xb0)
#define C_ACCENT RGB(0x40, 0xa0, 0xff)
#define C_OK     RGB(0x50, 0xd0, 0x70)
#define C_WARN   RGB(0xff, 0xb0, 0x30)
#define C_BAD    RGB(0xff, 0x50, 0x50)
#define C_BARBG  RGB(0x08, 0x0c, 0x18)

int ui_init(int off_x, int off_y)
{
    gs = gsKit_init_global();
    gs->PSM             = GS_PSM_CT24;
    gs->PSMZ            = GS_PSMZ_16S;
    gs->ZBuffering      = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8,
                1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);
    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);

    font = gsKit_init_fontm();
    if (font == NULL || gsKit_fontm_upload(gs, font) != 0)
        return -1;
    font->Spacing = 0.75f;

    ui_move(off_x, off_y);
    return 0;
}

void ui_move(int off_x, int off_y)
{
    g_off_x = off_x;
    g_off_y = off_y;
    gsKit_set_display_offset(gs, off_x, off_y);
}

int ui_off_x(void) { return g_off_x; }
int ui_off_y(void) { return g_off_y; }

/* ---- drawing helpers (coordinates relative to the safe area) */
static float X0, Y0, W;

static void text(float x, float y, float scale, u64 color, const char *s)
{
    gsKit_fontm_print_scaled(gs, font, X0 + x, Y0 + y, 2, scale, color, s);
}

static void rect(float x1, float y1, float x2, float y2, u64 color)
{
    gsKit_prim_sprite(gs, X0 + x1, Y0 + y1, X0 + x2, Y0 + y2, 1, color);
}

static void panel(float y1, float y2, const char *title)
{
    rect(0, y1, W, y2, C_PANEL);
    rect(0, y1, 4, y2, C_ACCENT);
    text(14, y1 + 6, 0.55f, C_DIM, title);
}

static const char *link_text(unsigned int m, u64 *color)
{
    *color = C_BAD;
    switch (m & 0x0f) {
        case 8: *color = C_OK; return "100 Mbit full duplex";
        case 4: return "100 Mbit HALF duplex - check cable/switch";
        case 2: return "10 Mbit full duplex - slow";
        case 1: return "10 Mbit half duplex - slow";
        default: *color = C_WARN; return "no link (waiting...)";
    }
}

static unsigned long long u64of(unsigned int lo, unsigned int hi) { return ((unsigned long long)hi << 32) | lo; }

void ui_draw(const px_stats_t *s, const char *cfg_src, float cur, float avg, const char *message, int calibrating)
{
    char b[160];
    u64 c;
    unsigned long long disk = u64of(s->disk_sectors_lo, s->disk_sectors_hi) * 512;
    float y;
    const float T = 0.48f, S = 0.40f; /* text and small text scale */

    X0 = gs->Width * 0.08f;
    Y0 = gs->Height * 0.07f;
    W  = gs->Width - 2 * X0;

    gsKit_clear(gs, C_BG);

    /* title */
    text(0, 0, 0.75f, C_TEXT, "ps2xfer");
    text(112, 6, S, C_DIM, "PC -> PS2 HDD over Ethernet");
    snprintf(b, sizeof(b), "%u.%u.%u.%u : %u", s->ip >> 24, (s->ip >> 16) & 255, (s->ip >> 8) & 255,
             s->ip & 255, s->port);
    text(0, 28, T, C_ACCENT, b);
    y = 52;

    /* network */
    panel(y, y + 56, "NETWORK");
    {
        const char *lt = link_text(s->link_mode, &c);
        text(14, y + 20, T, c, lt);
    }
    snprintf(b, sizeof(b), "MAC %02x:%02x:%02x:%02x:%02x:%02x   SPEED rev 0x%x", s->mac[0], s->mac[1], s->mac[2],
             s->mac[3], s->mac[4], s->mac[5], s->speed_rev);
    text(14, y + 38, S, C_DIM, b);
    y += 62;

    /* disk */
    panel(y, y + 56, "DISK");
    if (s->flags & PX_SF_DISK_OK) {
        snprintf(b, sizeof(b), "%.32s  %u.%u GB  UDMA %d", s->disk_model, (unsigned)(disk / 1000000000),
                 (unsigned)(disk / 100000000 % 10), s->udma_mode == 0xff ? -1 : (int)s->udma_mode);
        text(14, y + 20, T, C_TEXT, b);
        if (s->flags & PX_SF_FS_OK) {
            if (s->fs_free_mb != 0xffffffff)
                snprintf(b, sizeof(b), "exFAT  %u.%u GiB free of %u.%u GiB", s->fs_free_mb / 1024,
                         s->fs_free_mb % 1024 * 10 / 1024, s->fs_total_mb / 1024, s->fs_total_mb % 1024 * 10 / 1024);
            else
                snprintf(b, sizeof(b), "exFAT  %u.%u GiB", s->fs_total_mb / 1024, s->fs_total_mb % 1024 * 10 / 1024);
            text(14, y + 38, S, C_DIM, b);
        } else {
            text(14, y + 38, S, (s->flags & PX_SF_IOMANX) ? C_WARN : C_BAD,
                 (s->flags & PX_SF_IOMANX) ? "exFAT owned by bdmfs_fatfs (bench mode)" : "no exFAT volume found");
        }
    } else {
        text(14, y + 20, T, C_BAD, "internal HDD not found");
    }
    y += 62;

    /* transfer */
    panel(y, y + 112, "TRANSFER");
    if (s->flags & PX_SF_XFER) {
        unsigned long long size = u64of(s->file_size_lo, s->file_size_hi);
        unsigned long long done = u64of(s->written_lo, s->written_hi);
        float pct = size ? (float)done * 100.0f / (float)size : 0;
        float rate = avg > 0.05f ? avg : cur;
        int eta = rate > 0 ? (int)((float)(size - done) / (rate * 1e6f)) : 0;
        const char *fn = strrchr(s->file, '/');

        fn = fn ? fn + 1 : s->file;
        if (strlen(fn) > 12 && fn[4] == '_' && fn[8] == '.' && fn[11] == '.')
            fn += 12; /* OPL name: show it without the game ID */
        snprintf(b, sizeof(b), "%.40s", fn);
        text(14, y + 20, T, C_TEXT, b);
        rect(14, y + 42, W - 14, y + 62, C_BARBG);
        rect(14, y + 42, 14 + (W - 28) * pct / 100.0f, y + 62, C_ACCENT);
        snprintf(b, sizeof(b), "%.1f%%", pct);
        text(W / 2 - 24, y + 44, S, C_TEXT, b);
        snprintf(b, sizeof(b), "%llu / %llu MiB   %.2f MB/s   ETA %d:%02d:%02d", done >> 20, size >> 20, avg, eta / 3600,
                 eta / 60 % 60, eta % 60);
        text(14, y + 70, S, C_TEXT, b);
    } else {
        text(14, y + 24, T, (s->flags & PX_SF_SESSION) ? C_OK : C_DIM,
             (s->flags & PX_SF_SESSION) ? "Connected. Waiting for files." : "Waiting for the PC client...");
        text(14, y + 50, S, C_DIM, "On the PC run: py ps2xfer.py");
    }
    snprintf(b, sizeof(b), "pkts %u  retx %u  drop %u  rx err %u  csum %u  disk %u", s->rx_pkts, s->rx_retx,
             s->rx_drops, s->rx_errors, s->csum_errors, s->disk_errors);
    text(14, y + 90, S, (s->rx_errors || s->csum_errors || s->disk_errors) ? C_WARN : C_DIM, b);
    y += 120;

    /* footer */
    if (s->flags & PX_SF_ERROR)
        text(0, y, T, C_BAD, s->last_error);
    else if (message && *message)
        text(0, y, T, C_WARN, message);
    if (calibrating) {
        snprintf(b, sizeof(b), "ps2xfer.ini:  screen_x=%d  screen_y=%d", g_off_x, g_off_y);
        text(0, y + 20, T, C_ACCENT, b);
    } else {
        snprintf(b, sizeof(b), "START 2 s: exit   D-pad: move picture   IOP %u%%", s->cpu_load_x10 / 10);
        text(0, y + 20, S, C_DIM, b);
    }
    (void)cfg_src;

    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}
