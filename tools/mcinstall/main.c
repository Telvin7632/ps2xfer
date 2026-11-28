/*
 * mcinstall.elf - test helper, run from ps2link:
 *   ps2client execee host:mcinstall.elf <mc dir> <file>... [--icon <file>...] [--run <elf>]
 * Copies each host:<file> to <mc dir>/<file> on the memory card, reads it
 * back and compares, writes the read-back copy to host:<file>.card, and only
 * if every file matches starts <elf> (e.g. the copied ps2link).
 * Files after --icon (icon.sys and its icon) are copied only if <mc dir> had
 * no icon.sys: an existing folder keeps its own; a new one is not shown as a
 * damaged save by the browser. A report goes to host:mcinstall.log.
 * Used to put ps2link on the memory card so ps2xfer's exit_elf can return to it.
 */
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <sbv_patches.h>
#include <elf-loader.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

static char buf[64 * 1024] __attribute__((aligned(64)));
static char buf2[64 * 1024] __attribute__((aligned(64)));

/* compares two files; also writes b's content to copy_to; returns size or <0 */
static int verify(const char *a, const char *b, const char *copy_to)
{
    int fa = open(a, O_RDONLY), fb = open(b, O_RDONLY), fc, na, nb, total = 0;

    if (fa < 0 || fb < 0) {
        if (fa >= 0) close(fa);
        if (fb >= 0) close(fb);
        return -10;
    }
    fc = open(copy_to, O_WRONLY | O_CREAT | O_TRUNC);
    for (;;) {
        na = read(fa, buf, sizeof(buf));
        nb = read(fb, buf2, sizeof(buf2));
        if (na != nb || (na > 0 && memcmp(buf, buf2, na) != 0)) {
            total = -11;
            break;
        }
        if (na <= 0)
            break;
        if (fc >= 0)
            write(fc, buf2, nb);
        total += na;
    }
    close(fa);
    close(fb);
    if (fc >= 0)
        close(fc);
    return total;
}

static int copy(const char *src, const char *dst)
{
    int in = open(src, O_RDONLY), out, n, total = 0;

    if (in < 0)
        return -1;
    out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        return -2;
    }
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        if (write(out, buf, n) != n) {
            total = -3;
            break;
        }
        total += n;
    }
    close(in);
    close(out);
    return total;
}

static char report[2048];

static void say(const char *fmt, const char *a, const char *b, int n)
{
    char line[640];

    snprintf(line, sizeof(line), fmt, a, b, n);
    scr_printf("%s", line);
    strncat(report, line, sizeof(report) - strlen(report) - 1);
}

static void save_report(void)
{
    int fd = open("host:mcinstall.log", O_WRONLY | O_CREAT | O_TRUNC);

    if (fd >= 0) {
        write(fd, report, strlen(report));
        close(fd);
    }
}

int main(int argc, char *argv[])
{
    const char *run = NULL;
    char src[256], dst[256], back[256];
    int i, r, ok = 1, icon_part = 0, had_icon;

    SifInitRpc(0);
    init_scr();
    scr_printf("\n  mcinstall\n\n");
    /* ps2link normally has these already; a second load just fails */
    SifLoadModule("rom0:SIO2MAN", 0, NULL);
    SifLoadModule("rom0:MCMAN", 0, NULL);
    SifLoadModule("rom0:MCSERV", 0, NULL);

    if (argc < 3) {
        scr_printf("  usage: mcinstall <mc dir> <file>... [--run <elf>]\n");
        return 1;
    }
    snprintf(dst, sizeof(dst), "%s/icon.sys", argv[1]);
    r = open(dst, O_RDONLY);
    had_icon = r >= 0;
    if (r >= 0)
        close(r);
    say("  %s: icon.sys %s%d\n", argv[1], had_icon ? "already there " : "missing ", 0);
    mkdir(argv[1], 0777);
    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--run") == 0 && i + 1 < argc) {
            run = argv[++i];
            continue;
        }
        if (strcmp(argv[i], "--icon") == 0) {
            icon_part = 1;
            continue;
        }
        if (icon_part && had_icon) {
            say("  %s: kept the folder's own icon%s%d\n", argv[i], "", 0);
            continue;
        }
        snprintf(src, sizeof(src), "host:%s", argv[i]);
        snprintf(dst, sizeof(dst), "%s/%s", argv[1], argv[i]);
        r = copy(src, dst);
        say("  %s -> %s: %d\n", src, dst, r);
        snprintf(back, sizeof(back), "host:%s.card", argv[i]);
        r = (r > 0) ? verify(src, dst, back) : r;
        say("    verify %s%s: %d\n", "", "", r);
        if (r <= 0)
            ok = 0;
    }
    say("%s%s%d (1 = all files OK)\n", "  result: ", "", ok);
    save_report();
    if (!ok) {
        scr_printf("\n  FAILED: not starting anything. Reset the console.\n");
        SleepThread();
    }
    if (run != NULL) {
        scr_printf("  starting %s\n", run);
        sbv_patch_disable_prefix_check();
        LoadELFFromFile(run, 0, NULL); /* resets the IOP, loads from mc0 */
        scr_printf("  LoadELFFromFile failed\n");
        SleepThread();
    }
    return 0;
}
