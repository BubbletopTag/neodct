/* nd_recui.c -- one screen, one process, an answer on stdout.
 *
 * ============ THE SHELL KEEPS THE LOGIC ============
 *
 * ndsys-recovery.sh already separates "the part that can destroy a system"
 * from "the UI", and unit-tests the first half on the host
 * (neodct/tests/test_initramfs_recovery.py). That line is preserved exactly:
 * nd-recui never mounts, never writes a block device, never unzips. It draws
 * one screen, reads keys, prints the answer on stdout and exits -- which
 * makes it a drop-in for recovery_menu(), which already echoes an index.
 *
 *   nd-recui menu HEADING ITEM...     1-based index on stdout, or 0
 *   nd-recui confirm QUESTION         exit 0 = yes, 1 = no
 *   nd-recui message LINE...          waits for any key
 *   nd-recui progress --total N ...   copies stdin to stdout, counting
 *   nd-recui splash PATH [HOLD]       blits a .raw
 *
 * ============ EXIT 2 MEANS "USE THE TTY MENU" ============
 *
 * No framebuffer, or no input device. The shell falls back to the text menu
 * it has always had, unchanged. That is the whole error strategy, and it is
 * why nothing here tries to soldier on with half a UI.
 *
 * ============ progress IS A PIPE FILTER AND MUST NEVER TRUNCATE ============
 *
 * It sits in the middle of
 *
 *     unzip -p pkg rootfs.squashfs | nd-recui progress ... | dd of=$SYS_DEV
 *
 * so a byte it fails to pass on is a byte missing from the system partition.
 * Every failure that is not a stream failure therefore degrades to a plain
 * cat: no framebuffer, no keymap, a bad --total, all of them copy the data
 * and draw nothing. This is the one place in the program where "carry on
 * regardless" is the correct behaviour, and it is deliberate.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nd_recui.h"

#define ND_RECUI_FB      "/dev/fb0"
#define ND_RECUI_KEYMAP  "/mnt/user/keymap.json"
#define ND_RECUI_TITLE   "NeoDCT recovery"
#define ND_RECUI_COPY_SZ 65536

/* The strip below content_bottom, which the list's own clear deliberately
 * does not touch -- so a legend written there once survives every redraw.
 * That is the same guarantee nd_vlist_draw()'s 0..145 clear exists to give
 * callers, used for the one thing recovery has to say on every frame. */
#define ND_RECUI_LEGEND_Y (ND_RECUI_CONTENT_BOTTOM + 6)

static void on_signal(int sig)
{
    (void)sig;
    /* Async-signal-safe: open, ioctl, close and _exit only. Leaving the VT in
     * KD_GRAPHICS would give the shell a console that draws nothing. */
    nd_recvt_text();
    _exit(1);
}

static void install_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);

    /* progress is a pipeline stage; a downstream that closes early must give
     * us EPIPE to report, not a silent death that leaves the VT in graphics
     * mode and the caller none the wiser. */
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = SIG_IGN;
    (void)sigaction(SIGPIPE, &sa, NULL);
}

/* ------------------------------------------------------------------ *
 * Argument scanning
 * ------------------------------------------------------------------ *
 *
 * Hand-rolled rather than getopt_long, because `menu HEADING ITEM...` takes
 * arbitrary strings and a package filename read off a FAT card is not ours
 * to promise never starts with a dash. Options are recognised only before
 * the first positional argument, and `--` ends them explicitly.
 */

typedef struct {
    const char *keymap;
    const char *title;
    const char *step;
    const char *header;
    long long total;
    char *const *pos;
    int n_pos;
} nd_recargs;

static int parse_args(int argc, char **argv, nd_recargs *a)
{
    int i = 2; /* argv[1] is the verb */

    memset(a, 0, sizeof *a);
    a->keymap = ND_RECUI_KEYMAP;
    a->title = ND_RECUI_TITLE;
    a->step = "";
    a->total = -1;

    while (i < argc) {
        const char *opt = argv[i];

        if (strcmp(opt, "--") == 0) {
            i++;
            break;
        }
        if (opt[0] != '-')
            break;

        if (i + 1 >= argc) {
            fprintf(stderr, "nd-recui: %s needs a value\n", opt);
            return -1;
        }
        if (strcmp(opt, "--keymap") == 0) {
            a->keymap = argv[i + 1];
        } else if (strcmp(opt, "--title") == 0) {
            a->title = argv[i + 1];
        } else if (strcmp(opt, "--step") == 0) {
            a->step = argv[i + 1];
        } else if (strcmp(opt, "--header") == 0) {
            a->header = argv[i + 1];
        } else if (strcmp(opt, "--total") == 0) {
            char *end = NULL;

            a->total = strtoll(argv[i + 1], &end, 10);
            if (end == argv[i + 1] || *end != '\0')
                a->total = -1;
        } else {
            fprintf(stderr, "nd-recui: unknown option %s\n", opt);
            return -1;
        }
        i += 2;
    }

    a->pos = &argv[i];
    a->n_pos = argc - i;
    return 0;
}

/* ------------------------------------------------------------------ *
 * Screens
 * ------------------------------------------------------------------ */

/* The legend lives in the strip the list never clears, so it is written once
 * per screen rather than once per frame. */
static void draw_legend(nd_recfb *fb, const char *text)
{
    char line[ND_RECUI_ITEM_MAX];

    nd_recdraw_clear(fb, ND_RECUI_CONTENT_BOTTOM + 1, fb->h - 1, ND_RECCOL_BLACK);
    (void)nd_recdraw_text_fit(line, sizeof line, text, ND_RECFONT_SMALL, fb->w - 10);
    nd_recdraw_text(fb, 5, ND_RECUI_LEGEND_Y, line, ND_RECFONT_SMALL, ND_RECCOL_WHITE);
}

/* Title in the header band, heading below the divider is not available -- the
 * list owns those rows -- so the heading goes in the legend strip. On the
 * first screen that heading is "1-5 or arrows, Enter", which IS a legend. */
static void draw_header(nd_recfb *fb, const char *title)
{
    char line[ND_RECUI_ITEM_MAX];

    (void)nd_recdraw_text_fit(line, sizeof line, title, ND_RECFONT_LARGE, fb->w - 10);
    nd_recdraw_text(fb, 5, 0, line, ND_RECFONT_LARGE, ND_RECCOL_WHITE);
}

static int verb_menu(nd_recfb *fb, nd_recinput *in, const nd_recargs *a)
{
    const char *items[ND_RECUI_MAX_ITEMS];
    const char *heading;
    size_t n_items = 0;
    size_t selected = 0u;
    size_t window = 0u;
    int i;

    if (a->n_pos < 1) {
        fprintf(stderr, "nd-recui: menu needs a heading\n");
        return ND_RECUI_EXIT_NO_INPUT;
    }
    heading = a->pos[0];

    for (i = 1; i < a->n_pos && n_items < ND_RECUI_MAX_ITEMS; i++)
        items[n_items++] = a->pos[i];
    if (a->n_pos - 1 > ND_RECUI_MAX_ITEMS)
        fprintf(stderr, "nd-recui: %d items, showing the first %d\n", a->n_pos - 1,
                ND_RECUI_MAX_ITEMS);
    if (n_items == 0u) {
        fprintf(stderr, "nd-recui: menu needs at least one item\n");
        return ND_RECUI_EXIT_NO_INPUT;
    }

    draw_legend(fb, (heading[0] != '\0') ? heading : "arrows, then Enter");
    for (;;) {
        int32_t key;
        int32_t r;

        nd_reclist_draw(fb, a->title, items, n_items, selected, &window);
        draw_header(fb, a->title);

        key = nd_recinput_wait(in);
        if (key == ND_RECKEY_NONE)
            return ND_RECUI_EXIT_NO_INPUT;

        r = nd_reclist_key(key, n_items, &selected);
        if (r == ND_RECLIST_BACK) {
            /* recovery_menu()'s "or 0" -- nothing chosen. */
            printf("0\n");
            return 0;
        }
        if (r >= 0) {
            printf("%d\n", (int)r + 1);
            return 0;
        }
    }
}

/* Yes/no, with NOTHING preselected.
 *
 * recovery_confirm() in the shell defaults to "no"; here neither row is lit
 * until a key moves the selection, so a stray Enter on "WIPE SYSTEM?" cannot
 * answer it at all. That is a deliberate divergence from the tty menu and the
 * reason is the question: these are the two screens in recovery that destroy
 * something.
 */
static int verb_confirm(nd_recfb *fb, nd_recinput *in, const nd_recargs *a)
{
    const char *question;
    int selected = -1; /* nothing lit */

    if (a->n_pos < 1) {
        fprintf(stderr, "nd-recui: confirm needs a question\n");
        return 1;
    }
    question = a->pos[0];

    draw_legend(fb, "Up/Down, then Enter");

    for (;;) {
        int32_t key;

        nd_recconfirm_draw(fb, question, selected);

        key = nd_recinput_wait(in);
        if (key == ND_RECKEY_NONE)
            return 1; /* no input left: the safe answer is no */

        switch (key) {
        case ND_RECKEY_UP:
        case ND_RECKEY_DOWN:
            selected = (selected == 0) ? 1 : 0;
            break;
        case ND_RECKEY_1:
            selected = 0;
            break;
        case 3: /* ND_RECKEY_2 */
            selected = 1;
            break;
        case ND_RECKEY_CLEAR:
            return 1;
        case ND_RECKEY_ENTER:
            if (selected == 0)
                return 0;
            if (selected == 1)
                return 1;
            break; /* nothing chosen yet: Enter does nothing */
        default:
            break;
        }
    }
}

static int verb_message(nd_recfb *fb, nd_recinput *in, const nd_recargs *a)
{
    const char *lines[ND_RECUI_MAX_MSG_LINE];
    size_t n = 0u;
    int i;

    for (i = 0; i < a->n_pos && n < ND_RECUI_MAX_MSG_LINE; i++)
        lines[n++] = a->pos[i];

    nd_recdraw_clear(fb, ND_RECUI_CONTENT_BOTTOM + 1, fb->h - 1, ND_RECCOL_BLACK);
    nd_recmessage_draw(fb, lines, n);
    (void)nd_recinput_wait(in);
    return 0;
}

static int verb_splash(nd_recfb *fb, const nd_recargs *a)
{
    if (a->n_pos < 1) {
        fprintf(stderr, "nd-recui: splash needs a path\n");
        return 1;
    }
    if (nd_recdraw_blit_raw(fb, a->pos[0]) != 0) {
        fprintf(stderr, "nd-recui: cannot blit %s\n", a->pos[0]);
        return 1;
    }
    if (a->n_pos >= 2) {
        long hold = strtol(a->pos[1], NULL, 10);

        if (hold > 0 && hold < 3600)
            (void)sleep((unsigned)hold);
    }
    return 0;
}

/* stdin to stdout, counting bytes and moving a bar. See the file header:
 * every failure that is not a stream failure degrades to a plain cat. */
static int verb_progress(const nd_recargs *a)
{
    static uint8_t buf[ND_RECUI_COPY_SZ];
    nd_recfb fb;
    nd_recprogress p;
    bool have_fb;
    int64_t done = 0;
    int rc = 0;

    have_fb = (a->total > 0) && (nd_recfb_open(&fb, ND_RECUI_FB) == 0);
    if (have_fb) {
        nd_recvt_graphics();
        nd_recprogress_init(&p, &fb, a->step, a->header);
        (void)nd_recprogress_draw(&p, 0, a->total);
    }

    for (;;) {
        ssize_t got = read(STDIN_FILENO, buf, sizeof buf);
        size_t written = 0u;

        if (got < 0) {
            if (errno == EINTR)
                continue;
            fprintf(stderr, "nd-recui: read: %s\n", strerror(errno));
            rc = 1;
            break;
        }
        if (got == 0)
            break;

        while (written < (size_t)got) {
            ssize_t n = write(STDOUT_FILENO, buf + written, (size_t)got - written);

            if (n < 0) {
                if (errno == EINTR)
                    continue;
                fprintf(stderr, "nd-recui: write: %s\n", strerror(errno));
                rc = 1;
                break;
            }
            written += (size_t)n;
        }
        if (rc != 0)
            break;

        done += got;
        if (have_fb)
            (void)nd_recprogress_draw(&p, done, a->total);
    }

    if (have_fb) {
        /* The last chunk rarely lands on a whole percent, so the bar would
         * stop at 99% on a successful pass. Finish it -- a bar that never
         * reaches the end reads as a failure. */
        if (rc == 0)
            (void)nd_recprogress_draw(&p, a->total, a->total);
        nd_recvt_text();
        nd_recfb_close(&fb);
    }
    return rc;
}

/* ------------------------------------------------------------------ *
 * main
 * ------------------------------------------------------------------ */

static void usage(void)
{
    fprintf(stderr, "usage: nd-recui menu|confirm|message|progress|splash [options] [args]\n");
}

int main(int argc, char **argv)
{
    nd_recargs args;
    nd_recfb fb;
    nd_recinput in;
    const char *verb;
    int rc;

    if (argc < 2) {
        usage();
        return 1;
    }
    verb = argv[1];
    if (parse_args(argc, argv, &args) != 0)
        return 1;

    install_handlers();

    /* progress is the only verb that needs neither a screen nor a key, and
     * the only one that must never fail its caller. It opens its own
     * framebuffer, or does without. */
    if (strcmp(verb, "progress") == 0)
        return verb_progress(&args);

    if (nd_recfb_open(&fb, ND_RECUI_FB) != 0)
        return ND_RECUI_EXIT_NO_INPUT;

    if (strcmp(verb, "splash") == 0) {
        nd_recvt_graphics();
        rc = verb_splash(&fb, &args);
        nd_recvt_text();
        nd_recfb_close(&fb);
        return rc;
    }

    /* Everything below reads keys, so a phone with no reachable keypad has to
     * say so BEFORE it draws: a menu nobody can move is worse than the
     * console text that at least works over a serial cable. */
    if (nd_recinput_open(&in, args.keymap) != 0) {
        fprintf(stderr, "nd-recui: no keypad and no input device; use the serial console\n");
        nd_recfb_close(&fb);
        return ND_RECUI_EXIT_NO_INPUT;
    }

    nd_recvt_graphics();
    if (strcmp(verb, "menu") == 0)
        rc = verb_menu(&fb, &in, &args);
    else if (strcmp(verb, "confirm") == 0)
        rc = verb_confirm(&fb, &in, &args);
    else if (strcmp(verb, "message") == 0)
        rc = verb_message(&fb, &in, &args);
    else {
        usage();
        rc = 1;
    }
    nd_recvt_text();

    /* CODING-STANDARDS section 1.7: a clean teardown even at exit, so a leak
     * detector stays useful. */
    nd_recinput_close(&in);
    nd_recfb_close(&fb);
    return rc;
}
