/* nd_easteregg.c -- the thing that happens if you dial the thing.
 *
 * See nd_easteregg.h. Deliberately self-contained: nothing calls into it
 * except the one check in nd_ui.c's dial handler, and it calls out only to
 * the drawing primitives and nd_proc_spawn().
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "nd_draw.h"
#include "nd_easteregg.h"
#include "nd_media.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_types.h"
#include "nd_ui.h"

/* ------------------------------------------------------------------ *
 * The code
 * ------------------------------------------------------------------ */

bool nd_easteregg_is_code(const char *dialed)
{
    if (dialed == NULL)
        return false;
    return strcmp(dialed, ND_EASTEREGG_CODE) == 0;
}

/* ------------------------------------------------------------------ *
 * Official-looking nonsense
 * ------------------------------------------------------------------ */

/* A cheap deterministic scrambler, so line N always reads the same.
 *
 * Not rand(): this must not disturb the process-wide sequence anything else
 * might be using, and it must be reproducible in the test without seeding.
 * Knuth's LCG constants, iterated `rounds` times so one line number yields
 * several unrelated-looking numbers. */
static uint32_t scramble(uint32_t n, uint32_t rounds)
{
    uint32_t x = n * 2654435761u + 1013904223u;
    uint32_t i;

    for (i = 0u; i < rounds; i++)
        x = x * 1664525u + 1013904223u;
    return x;
}

/* How many shapes below. Bumping this without adding a case is caught by the
 * default arm, which is a real line rather than a fallback nobody sees. */
#define EGG_SHAPES 20u

/* The format strings are LITERALS in each arm rather than entries in a table
 * the switch indexes. A table would be tidier to look at and would put the
 * format one lookup away from the arguments it is called with, which is the
 * one mistake in here the compiler could otherwise catch for us. */
void nd_easteregg_line(uint32_t n, char *out, size_t out_sz)
{
    uint32_t a = scramble(n, 1u);
    uint32_t b = scramble(n, 2u);

    if (out == NULL || out_sz == 0u)
        return;
    out[0] = '\0';

    switch (scramble(n, 0u) % EGG_SHAPES) {
    case 0:
        (void)nd_snprintf(out, out_sz, "[%2u.%03u] ubi0: vol %u ok", a % 60u, b % 1000u, a % 8u);
        break;
    case 1:
        (void)nd_snprintf(out, out_sz, "sha256 %08x verified", a);
        break;
    case 2:
        (void)nd_snprintf(out, out_sz, "map 0x%08x +%u KiB", a, b % 512u);
        break;
    case 3:
        (void)nd_snprintf(out, out_sz, "verity: gate %u bypassed", a % 16u);
        break;
    case 4:
        (void)nd_snprintf(out, out_sz, "rv1103 pin %u -> %u", a % 128u, b % 2u);
        break;
    case 5:
        (void)nd_snprintf(out, out_sz, "eth0 up, %u pkt, %u err", a % 9999u, b % 4u);
        break;
    case 6:
        (void)nd_snprintf(out, out_sz, "decode frame %u (%u b)", a % 6000u, b % 65535u);
        break;
    case 7:
        (void)nd_snprintf(out, out_sz, "cache %08x -> %08x", a, b);
        break;
    case 8:
        (void)nd_snprintf(out, out_sz, "ioctl 0x%04x = %d", a % 65535u, (int)(b % 3u));
        break;
    case 9:
        (void)nd_snprintf(out, out_sz, "trace pc=%08x lr=%08x", a, b);
        break;
    case 10:
        (void)nd_snprintf(out, out_sz, "unlock sector %u/512", a % 512u);
        break;
    case 11:
        (void)nd_snprintf(out, out_sz, "rot%u: %u blocks done", a % 26u, b % 4096u);
        break;
    case 12:
        (void)nd_snprintf(out, out_sz, "heap %u KiB free", a % 20000u);
        break;
    case 13:
        (void)nd_snprintf(out, out_sz, "thread %u joined", a % 64u);
        break;
    case 14:
        (void)nd_snprintf(out, out_sz, "scan bank %u ... ok", a % 32u);
        break;
    case 15:
        (void)nd_snprintf(out, out_sz, "nand: erase blk %u", a % 8192u);
        break;
    case 16:
        (void)nd_snprintf(out, out_sz, "dma ch%u -> 0x%08x", a % 8u, b);
        break;
    case 17:
        (void)nd_strlcpy(out, "ACCESS GRANTED", out_sz);
        break;
    case 18:
        (void)nd_snprintf(out, out_sz, "payload staged (%u b)", a % 60000u);
        break;
    default:
        (void)nd_snprintf(out, out_sz, "handshake %u/8 ok", (a % 8u) + 1u);
        break;
    }
}

/* ------------------------------------------------------------------ *
 * The scroll
 * ------------------------------------------------------------------ */

static void egg_nap(double seconds)
{
    struct timespec ts;

    if (seconds <= 0.0)
        return;
    ts.tv_sec = (time_t)seconds;
    ts.tv_nsec = (long)((seconds - (double)ts.tv_sec) * 1e9);
    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

static void draw_rows(nd_ui *ui, char rows[][ND_EGG_LINE_MAX], size_t n_rows)
{
    size_t i;

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, nd_ui_width(ui), nd_ui_height(ui)), ND_BLACK);
    for (i = 0u; i < n_rows; i++)
        (void)nd_draw_text(ui->draw, 2, (int32_t)i * ND_EGG_PITCH, rows[i], ui->font_s, ND_WHITE);
}

static void scroll_the_logs(nd_ui *ui)
{
    char rows[ND_EGG_ROWS][ND_EGG_LINE_MAX];
    size_t n_rows = 0u;
    uint32_t emitted = 0u;

    if (ui->font_s == NULL)
        return;

    while (emitted < (uint32_t)ND_EGG_LINES_TOTAL) {
        uint32_t k;

        for (k = 0u; k < (uint32_t)ND_EGG_LINES_PER_FRAME &&
                     emitted < (uint32_t)ND_EGG_LINES_TOTAL;
             k++, emitted++) {
            if (n_rows == (size_t)ND_EGG_ROWS) {
                /* memmove and not memcpy: the regions overlap, which is the
                 * whole point of a scroll. */
                memmove(rows[0], rows[1], (size_t)(ND_EGG_ROWS - 1) * ND_EGG_LINE_MAX);
                n_rows--;
            }
            nd_easteregg_line(emitted, rows[n_rows], ND_EGG_LINE_MAX);
            n_rows++;
        }
        draw_rows(ui, rows, n_rows);
        if (nd_ui_present(ui) != ND_OK)
            return;
        egg_nap(ND_EGG_FRAME_S);
    }
}

/* ------------------------------------------------------------------ *
 * The video
 * ------------------------------------------------------------------ */

/* --no-suspend, and then wait.
 *
 * neodct-play's default is to SIGSTOP its parent for the duration, which is
 * right for the browser -- netsurf would otherwise keep drawing over the
 * video -- and wrong here: the parent is the core, and a stopped core cannot
 * reap the child that is meant to continue it. Blocking in nd_proc_wait()
 * gets the same result, because a core sitting in waitpid is a core that is
 * not drawing. */
static void play_the_video(void)
{
    char resolved[ND_PATH_MAX];
    char player[ND_PATH_MAX];
    const char *argv[4];
    nd_proc_spec spec;
    nd_proc_status status;
    pid_t pid = -1;

    /* Absent is the ordinary case on a build that did not ship the file, and
     * on QEMU. Nothing is said about it. */
    if (nd_path_resolve(resolved, sizeof resolved, ND_EASTEREGG_VIDEO) != ND_OK)
        return;
    if (access(resolved, R_OK) != 0)
        return;
    if (nd_path_resolve(player, sizeof player, ND_MEDIA_PLAYER) != ND_OK)
        return;
    if (access(player, X_OK) != 0)
        return;

    argv[0] = player;
    argv[1] = "--no-suspend";
    argv[2] = resolved;
    argv[3] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    /* If the core dies while this plays, the player must not be left holding
     * the framebuffer -- the same failure the death_signal block in nd_proc.h
     * describes for an orphaned app. */
    spec.death_signal = SIGTERM;

    if (nd_proc_spawn(player, &spec, &pid) != ND_OK || pid <= 0)
        return;

    /* A bound rather than forever: a player wedged on a decode must not take
     * the phone with it. */
    (void)nd_proc_wait(pid, ND_EGG_PLAY_TIMEOUT_S, &status);
}

void nd_easteregg_run(nd_ui *ui)
{
    if (ui == NULL)
        return;
    scroll_the_logs(ui);
    play_the_video();
}
