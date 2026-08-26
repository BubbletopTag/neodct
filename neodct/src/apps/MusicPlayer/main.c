/* apps/MusicPlayer/main.c -- the "NeoPod", app id 970.
 *
 * A one-to-one port of System/apps/MusicPlayer/main.py (516 lines): the
 * no-card screens, the track list, and the split-layout "Now Playing" screen
 * with cover art on the left and the tags on the right. The card scan, the
 * ID3 reader and the two playback backends live in meta.c and audio.c.
 *
 * golden/app-musicplayer.png is NONE of that. The harness has no SD card, so
 * `run(ui)` gets four lines in and stops:
 *
 *     if app.music_dir() is None:
 *         MessageDialog(ui, "No SD card.\nMusic is played from a card.",
 *                       button_text="More").show()
 *         TextScroller(ui, NO_CARD_HELP).show()
 *         return
 *
 * and the reference frame is that MessageDialog, with "More" on the softkey.
 * Everything below it is checked by test_musicplayer.c instead.
 *
 * ============ FIVE THINGS THAT LOOK WRONG AND ARE PORTED ANYWAY ============
 *
 * 1. THE BREADCRUMB READS "4-N" IN AN APP WHOSE ID IS 970. The track list is
 *    built as `VerticalList(ui, "Music", display_list, app_id=4)`, and 4 is
 *    Settings' root id. See music.h and OPEN-QUESTIONS.md MU-2.
 *
 * 2. THE TEXT COLUMN IS 27 px FURTHER RIGHT THAN THE ARTWORK NEEDS. `text_x`
 *    is computed as art_x + art_size + 8 while art_size is still 100, and
 *    art_size is only then reduced to 73 so the artwork clears the progress
 *    bar. So the intended 8 px gutter is really 35: art ends at x=81 and the
 *    text starts at x=116. Reproduced, because moving the text column moves
 *    every label on the screen.
 *
 * 3. THE EMPTY-STATE HINT NAMES A DIRECTORY THAT IS NOT USED. "Add mp3s to:
 *    /User/music" -- music has lived on the card at /NeoDCT/User/sdcard/music
 *    since Storage.folder() was introduced. The string is on screen, so it is
 *    ported verbatim.
 *
 * 4. THE ARTWORK IS RESIZED TWICE, NEAREST BOTH TIMES. Once to 100x100 and
 *    then to 73x73 -- not once from the original to 73x73. Two nearest-
 *    neighbour passes do not compose into one, so the pixels differ, and this
 *    is a screen with a picture on it.
 *
 * 5. `truncate()` MEASURES THE BARE STRING ONCE AND `s + "..."` FOREVER
 *    AFTER. Not a bug -- it is what lets a string that already fits come back
 *    without an ellipsis -- but it is an asymmetry a rewrite would iron out
 *    and the pixels would move. In meta.c, pinned by vectors taken from
 *    Pillow.
 *
 * ============ WHAT IS NO LONGER A PORT ============
 *
 * Everything above describes the port. Three things have since been added
 * that the Python never had, and they are marked as such wherever they
 * appear:
 *
 *   THE LIBRARY BROWSER   Artists / Albums / Songs, built from ID3 tags and
 *                         sorted properly. OPEN-QUESTIONS.md MU-11 recorded
 *                         that the Python's per-directory byte-value ordering
 *                         was ported deliberately and README.md called it
 *                         limited; this is the layer on top. nd_music_scan()
 *                         is untouched and FOLDERS still shows exactly what
 *                         it always did, which is what test_scan still pins.
 *
 *   VOLUME                music.h explains why it is a multiply and not an
 *                         amixer call. UP and DOWN in Now Playing, which are
 *                         the keys the MediaWidget already uses for it, open
 *                         a full-screen ten-segment scale; the same screen is
 *                         on the menu so the level can be set before anything
 *                         plays.
 *
 *   A QUEUE               picking a track from a list plays THAT LIST, and
 *                         the screen advances at the end of each track
 *                         instead of dropping back to the menu. An album you
 *                         cannot listen to without pressing a key every three
 *                         minutes is not really an album view.
 *
 * The golden frame is unaffected: it is the no-card dialog, which is reached
 * before any of this.
 *
 * ============ WHAT REPLACED miniaudio, AND WHY ============
 *
 * See the header of audio.c and docs/c-rewrite/AUDIO.md. In one line: the
 * same dr_mp3 / dr_wav decoders lib/nd_notify.c already streams the ringtone
 * with, feeding `aplay`, because neither miniaudio nor a 24 MB mpv process
 * belongs in the common path on a 53 MB phone.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "music.h"

/* The three hex colours the Python names, and nothing else on this screen
 * uses a colour that is not black or white. */
#define MUSIC_ARTIST_GREY ND_RGB(0xCC, 0xCC, 0xCC) /* "#cccccc" */
#define MUSIC_ALBUM_GREY  ND_RGB(0x99, 0x99, 0x99) /* "#999999" */
#define MUSIC_BAR_GREY    ND_RGB(0x33, 0x33, 0x33) /* "#333333" */

/* One row of the track list, as VerticalList wants it: an array of pointers
 * into a block of basenames. 256 * 256 = 65,536 bytes for the paths plus
 * 2,048 bytes of pointers, heap, freed before run() returns. */
typedef struct {
    nd_music_track *tracks;
    char (*names)[ND_MUSIC_PATH_MAX];
    const char **name_ptrs;
    size_t n;
} playlist;

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

static void playlist_free(playlist *p)
{
    free(p->tracks);
    free(p->names);
    free(p->name_ptrs);
    memset(p, 0, sizeof *p);
}

/* scan_music() plus the `display_list = [os.path.basename(p) ...]` that
 * always follows it. False only on allocation failure, which leaves an empty
 * list -- the same screen a card with no music gets. */
static bool playlist_load(playlist *p, nd_music_backend backend)
{
    size_t i;

    memset(p, 0, sizeof *p);

    /* owned by the caller; released with playlist_free() */
    p->tracks = calloc((size_t)ND_MUSIC_MAX, sizeof *p->tracks);
    p->names = calloc((size_t)ND_MUSIC_MAX, sizeof *p->names);
    p->name_ptrs = calloc((size_t)ND_MUSIC_MAX, sizeof *p->name_ptrs);
    if (p->tracks == NULL || p->names == NULL || p->name_ptrs == NULL) {
        playlist_free(p);
        return false;
    }

    p->n = nd_music_scan(p->tracks, (size_t)ND_MUSIC_MAX, backend);
    for (i = 0u; i < p->n; i++) {
        (void)nd_strlcpy(p->names[i], basename_of(p->tracks[i].path), ND_MUSIC_PATH_MAX);
        p->name_ptrs[i] = p->names[i];
    }
    return true;
}

/* ------------------------------------------------------------------ *
 * The empty state
 * ------------------------------------------------------------------ */

/* `while True: k = self.ui.wait_for_key(); if k in (14, 28): return`. Every
 * other key is ignored and NOTHING is redrawn. */
static void show_no_music(nd_ui *ui, nd_softkey *bar)
{
    int32_t screen_w = nd_ui_width(ui);
    int32_t content_bottom = nd_ui_content_bottom(ui);
    int32_t y = nd_max32(12, nd_trunc32((double)content_bottom * 0.35));

    /* rectangle((0, 0, screen_w, content_bottom)) -- INCLUSIVE, so row 145
     * is painted black and the softkey update below then repaints 145..175
     * over it. Pillow clips column 240 away; so does nd_draw. */
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);
    (void)nd_draw_text(ui->draw, 10, y, "No Music Found", ui->font_n, ND_WHITE);
    (void)nd_draw_text(ui->draw, 10, y + 30, "Add mp3s to:", ui->font_s, ND_GRAY);
    /* Stale, and on screen. See note 3 in the file header. */
    (void)nd_draw_text(ui->draw, 10, y + 50, "/User/music", ui->font_s, ND_GRAY);
    nd_softkey_update(bar, "Exit", true);

    for (;;) {
        int32_t k = nd_ui_wait_for_key(ui);

        if (k == ND_KEY_CLEAR || k == ND_KEY_ENTER)
            return;
        /* Not in the Python, which had an exception to unwind it. nd_app.h:
         * a loop that outlives a frame polls this. */
        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * The queue -- NOT a port
 * ------------------------------------------------------------------ *
 *
 * Picking a track plays the LIST it came from. The items borrow their
 * strings from whichever structure produced them -- the library's song array
 * or the folder playlist's name block -- both of which outlive the Now
 * Playing screen by construction, because the screen is called from inside
 * the loop that owns them. Nothing here copies a path.
 */

typedef struct {
    const char *path;
    const char *title;
} queue_item;

typedef struct {
    queue_item *items; /* heap: ND_MUSIC_MAX * 16 bytes at the very most */
    size_t n;
    size_t pos;
} music_queue;

static void queue_free(music_queue *q)
{
    free(q->items);
    memset(q, 0, sizeof *q);
}

static bool queue_alloc(music_queue *q, size_t n)
{
    memset(q, 0, sizeof *q);
    if (n == 0u)
        return true;
    q->items = calloc(n, sizeof *q->items);
    return q->items != NULL;
}

/* ------------------------------------------------------------------ *
 * A list of borrowed strings, which is what VerticalList wants
 * ------------------------------------------------------------------ */

/* Labels are formatted into a block rather than pointed at the library,
 * because a row often needs more than one field ("3  Sour Times").
 *
 * VerticalList DOES NOT ELLIPSIZE ITS ITEMS -- nd_vlist.c fits only the
 * TITLE, and draws each row with a plain nd_draw_text at x=10 -- so a long
 * album name runs off the right edge and under the scrollbar. Every label
 * therefore goes through nd_music_truncate() on the way in, at the same
 * font the widget draws it with. */
#define MUSIC_LABEL_MAX 64

/* x=10 to the scrollbar at screen_w-5, less a little air. nd_vlist.c:144
 * puts bar_x at screen_w - 5 and nd_vlist.c:167 draws the item at x=10. */
#define MUSIC_LABEL_W 215

typedef struct {
    char (*labels)[MUSIC_LABEL_MAX];
    const char **ptrs;
    size_t n;
} music_list;

static void music_list_free(music_list *l)
{
    free(l->labels);
    free(l->ptrs);
    memset(l, 0, sizeof *l);
}

static bool music_list_alloc(music_list *l, size_t n)
{
    memset(l, 0, sizeof *l);
    if (n == 0u)
        return true;
    l->labels = calloc(n, sizeof *l->labels);
    l->ptrs = calloc(n, sizeof *l->ptrs);
    if (l->labels == NULL || l->ptrs == NULL) {
        music_list_free(l);
        return false;
    }
    return true;
}

/* The font VerticalList actually draws an item with -- nd_vlist.c:136 falls
 * back to font_n when font_md failed to load, and measuring with the wrong
 * one would trim to the wrong width. */
static const nd_font *list_font(const nd_ui *ui)
{
    return (ui->font_md != NULL) ? ui->font_md : ui->font_n;
}

static void music_list_set(music_list *l, const nd_ui *ui, size_t i, const char *text)
{
    (void)nd_music_truncate(l->labels[i], MUSIC_LABEL_MAX, text, list_font(ui), MUSIC_LABEL_W);
    l->ptrs[i] = l->labels[i];
}

/* ------------------------------------------------------------------ *
 * Building the library, behind a progress bar
 * ------------------------------------------------------------------ */

typedef struct {
    nd_ui *ui;
    nd_progress bar;
    bool cancelled;
} build_ctx;

/* Returns false to cancel. Clear is polled rather than waited on, which is
 * the only reason a scan of a full card can be interrupted at all. */
static bool build_progress(void *ctx, size_t done, size_t total)
{
    build_ctx *b = ctx;

    (void)nd_progress_draw(&b->bar, (int64_t)done, (int64_t)total);
    if (nd_app_should_exit()) {
        b->cancelled = true;
        return false;
    }
    if (nd_ui_read_keypress(b->ui, 0.0) == ND_KEY_CLEAR) {
        b->cancelled = true;
        return false;
    }
    return true;
}

/* The session's library, built at most once. NULL until something asks for
 * it; owned here and released at the end of app_run().
 *
 * A file-static for the same reason audio.c's player is: there is one Music
 * app in this process, and threading it through nine screens would buy
 * nothing. */
static nd_music_library *g_library;
static bool g_library_tried;

/* Builds it if it is not there. False means "there is nothing to browse" --
 * either the build failed, the user cancelled, or the card is empty -- and
 * the caller has already been told why on screen where it matters. */
static bool ensure_library(nd_ui *ui)
{
    playlist p;
    build_ctx b;
    nd_err rc;

    if (g_library != NULL)
        return nd_music_library_n_songs(g_library) > 0u;
    if (g_library_tried)
        return false;
    g_library_tried = true;

    if (!playlist_load(&p, nd_music_backend_now()))
        return false;
    if (p.n == 0u) {
        playlist_free(&p);
        return false;
    }

    memset(&b, 0, sizeof b);
    b.ui = ui;
    nd_progress_init(&b.bar, ui, "Reading tags", "Music", "Clear to stop", NULL, NULL);
    (void)nd_progress_draw(&b.bar, 0, (int64_t)p.n);

    rc = nd_music_library_build(&g_library, p.tracks, p.n, build_progress, &b);
    playlist_free(&p);

    if (rc != ND_OK) {
        if (rc != ND_ERR_BUSY)
            nd_log_err(ND_LOG_MUSIC, "could not build the library: %s", nd_strerror(rc));
        /* A cancel is not a failure and must be retryable: the next visit
         * scans again rather than remembering a refusal. */
        g_library_tried = false;
        return false;
    }
    return nd_music_library_n_songs(g_library) > 0u;
}

/* ------------------------------------------------------------------ *
 * Now Playing
 * ------------------------------------------------------------------ */

/* The whole screen, drawn from the values run_now_playing() computed once.
 * Split out only because the loop around it is already the longest function
 * in the app; every coordinate is the Python's. */
typedef struct {
    int32_t screen_w;
    int32_t content_bottom;
    int32_t header_h;
    int32_t art_x;
    int32_t art_y;
    int32_t art_size;
    int32_t text_x;
    int32_t text_width;
    int32_t bar_x;
    int32_t bar_y;
    int32_t bar_width;
} now_layout;

static void draw_now_playing(nd_ui *ui, nd_softkey *bar, const now_layout *L,
                             const nd_music_meta *meta, const nd_image *art,
                             double current_elapsed)
{
    char line[ND_MUSIC_TEXT_MAX + 8];
    char stamp[32];
    int32_t w = 0;
    int32_t h = 0;
    double pct;
    int32_t fill_width;

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, L->screen_w, L->content_bottom), ND_BLACK);

    /* -- Header -- */
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, L->screen_w, L->header_h), ND_WHITE);
    nd_text_size(ui->font_s, "Now Playing", &w, &h);
    (void)nd_draw_text(ui->draw, (L->screen_w - w) / 2, nd_max32(2, (L->header_h - h) / 2),
                       "Now Playing", ui->font_s, ND_BLACK);

    /* -- Album art (left) -- */
    if (art != NULL) {
        /* canvas.paste(display_art, (art_x, art_y)) with NO mask: PIL
         * converts to the destination mode and DROPS alpha rather than
         * compositing it, which is what nd_image_blit does. */
        (void)nd_image_blit(ui->canvas, art, L->art_x, L->art_y);
        (void)nd_draw_rect_outline(ui->draw,
                                   ND_RECT(L->art_x - 1, L->art_y - 1, L->art_x + L->art_size,
                                           L->art_y + L->art_size),
                                   ND_WHITE, 1);
    } else {
        int32_t cx = L->art_x + (L->art_size / 2);
        int32_t cy = L->art_y + (L->art_size / 2);

        (void)nd_draw_rect_outline(
            ui->draw,
            ND_RECT(L->art_x, L->art_y, L->art_x + L->art_size, L->art_y + L->art_size), ND_WHITE,
            1);
        /* The note: a filled head, a stem and a flag. A wide line grows in
         * the MINOR AXIS ONLY, so width 2 on the vertical stem lights columns
         * cx+1 and cx+2 -- nd_draw.h rule 2. */
        (void)nd_draw_ellipse_fill(ui->draw, ND_RECT(cx - 8, cy + 8, cx + 1, cy + 17), ND_WHITE);
        (void)nd_draw_line(ui->draw, cx + 1, cy + 12, cx + 1, cy - 12, ND_WHITE, 2);
        (void)nd_draw_line(ui->draw, cx + 1, cy - 12, cx + 14, cy - 8, ND_WHITE, 2);
    }

    /* -- Info (right) -- */
    (void)nd_music_truncate(line, sizeof line, meta->title, ui->font_n, L->text_width);
    (void)nd_draw_text(ui->draw, L->text_x, L->art_y, line, ui->font_n, ND_WHITE);

    (void)nd_music_truncate(line, sizeof line, meta->artist, ui->font_s, L->text_width);
    (void)nd_draw_text(ui->draw, L->text_x, L->art_y + 25, line, ui->font_s, MUSIC_ARTIST_GREY);

    if (meta->album[0] != '\0') {
        (void)nd_music_truncate(line, sizeof line, meta->album, ui->font_s, L->text_width);
        (void)nd_draw_text(ui->draw, L->text_x, L->art_y + 45, line, ui->font_s, MUSIC_ALBUM_GREY);
    }

    /* -- Progress bar -- */
    (void)nd_draw_rect_fill(
        ui->draw, ND_RECT(L->bar_x, L->bar_y, L->bar_x + L->bar_width, L->bar_y + 4),
        MUSIC_BAR_GREY);

    if (meta->length > 0.0) {
        pct = current_elapsed / meta->length;
        if (pct > 1.0)
            pct = 1.0;
    } else {
        pct = 0.0;
    }
    /* int(bar_width * pct): Pillow and Python both truncate toward zero. */
    fill_width = nd_trunc32((double)L->bar_width * pct);
    (void)nd_draw_rect_fill(ui->draw,
                            ND_RECT(L->bar_x, L->bar_y, L->bar_x + fill_width, L->bar_y + 4),
                            ND_WHITE);

    /* -- Timestamps -- */
    nd_music_format_time(nd_trunc32(current_elapsed), stamp, sizeof stamp);
    (void)nd_draw_text(ui->draw, L->bar_x, L->bar_y - 15, stamp, ui->font_s, ND_WHITE);

    if (meta->length > 0.0) {
        char total[40];
        double left = meta->length - current_elapsed;

        if (left < 0.0)
            left = 0.0;
        nd_music_format_time(nd_trunc32(left), stamp, sizeof stamp);
        (void)nd_snprintf(total, sizeof total, "-%s", stamp);
        nd_text_size(ui->font_s, total, &w, &h);
        (void)nd_draw_text(ui->draw, L->bar_x + L->bar_width - w, L->bar_y - 15, total, ui->font_s,
                           ND_WHITE);
    }

    nd_softkey_update(bar, nd_music_is_paused() ? "Play" : "Pause", true);
}

/* meta->art, resized the way the Python resizes it. Owned by the caller. */
static nd_image *prepare_art(const nd_music_meta *meta, int32_t first_size, int32_t final_size)
{
    nd_image *first;
    nd_image *out;

    if (meta->art == NULL)
        return NULL;

    /* `art.draft("RGB", (art_size*2, art_size*2)); art.load();
     *  display_art = art.resize((art_size, art_size), NEAREST)`.
     *
     * draft() is a JPEG-only DCT-scaling hint that changes speed and memory
     * and not the final NEAREST-resampled pixels; it has no equivalent here
     * and is skipped, which spec-apps-core.md section 10g explicitly allows. */
    first = nd_image_resize_nearest(meta->art, first_size, first_size);
    if (first == NULL)
        return NULL;
    if (first_size == final_size)
        return first;

    /* THE SECOND PASS IS FROM THE FIRST RESULT, not from the original. Two
     * nearest-neighbour passes do not compose into one. See note 4. */
    out = nd_image_resize_nearest(first, final_size, final_size);
    nd_image_free(first);
    return out;
}

/* Defined below, next to the rest of the browser. Declared here because Now
 * Playing opens it on a volume key and sits above it in the file. */
static void volume_screen(nd_ui *ui);

/* How one track's Now Playing screen ended. */
typedef enum {
    NP_BACK = 0, /* Clear: leave the queue entirely */
    NP_NEXT,     /* the track finished, or Right was pressed */
    NP_PREV
} np_result;

static np_result run_now_playing(nd_ui *ui, nd_softkey *bar, const char *filepath)
{
    now_layout L;
    nd_music_meta meta;
    nd_image *art = NULL;
    int32_t screen_w = nd_ui_width(ui);
    int32_t screen_h = nd_ui_height(ui);
    int32_t content_bottom = nd_ui_content_bottom(ui);
    int32_t header_h = nd_max32(24, nd_trunc32((double)screen_h * 0.08));
    int32_t art_size;
    int32_t available_media_h;
    int32_t lw = 0;
    int32_t lh = 0;
    double start_time;
    double paused_at = 0.0;
    double total_paused = 0.0;
    bool needs_redraw = true;
    np_result result = NP_BACK;

    /* 1. The loading card. Note it clears the WHOLE screen including the
     *    softkey strip, and presents directly rather than through the bar. */
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, screen_h), ND_BLACK);
    nd_text_size(ui->font_n, "Loading...", &lw, &lh);
    (void)nd_draw_text(ui->draw, (screen_w - lw) / 2, nd_max32(10, (content_bottom - lh) / 2),
                       "Loading...", ui->font_n, ND_WHITE);
    (void)nd_ui_present(ui);

    /* The FULL metadata -- duration and artwork included -- for one track,
     * which is what this screen needs and what the library scan deliberately
     * does not do for all of them. See the library note in music.h. */
    nd_music_get_metadata(filepath, &meta);

    /* 2. Geometry. THE ORDER OF THESE SIX LINES IS THE BUG IN NOTE 2:
     *    text_x and text_width are fixed while art_size is still 100. */
    art_size = nd_min32(100, nd_max32(64, nd_trunc32((double)screen_w * 0.42)));

    L.screen_w = screen_w;
    L.content_bottom = content_bottom;
    L.header_h = header_h;
    L.art_x = 8;
    L.art_y = header_h + 12;
    L.text_x = L.art_x + art_size + 8;
    L.text_width = nd_max32(30, screen_w - L.text_x - 8);
    L.bar_width = nd_max32(48, screen_w - 20);
    L.bar_x = (screen_w - L.bar_width) / 2;
    L.bar_y = content_bottom - 18;

    /* Keep art fully above progress/timestamps. */
    available_media_h = nd_max32(48, L.bar_y - L.art_y - 18);
    L.art_size = nd_min32(art_size, available_media_h);

    art = prepare_art(&meta, art_size, L.art_size);

    start_time = nd_time_now();

    for (;;) {
        double now;
        double current_elapsed;
        double pos;
        int32_t key;

        if (nd_music_is_finished()) {
            nd_music_stop();
            /* The track ran out: go on to the next one rather than dropping
             * the user back to a menu. NOT a port -- see the file header. */
            result = NP_NEXT;
            break;
        }

        /* The wall-clock fallback, kept for the backend that has no position
         * to report. `paused_at == 0` really is the Python's "not paused"
         * sentinel, and it is a float compared against a literal zero. */
        now = nd_time_now();
        if (nd_music_is_paused()) {
            if (paused_at == 0.0)
                paused_at = now;
            current_elapsed = paused_at - start_time - total_paused;
        } else {
            if (paused_at != 0.0) {
                total_paused += (now - paused_at);
                paused_at = 0.0;
            }
            current_elapsed = now - start_time - total_paused;
        }
        if (nd_music_position(&pos))
            current_elapsed = pos;

        if (needs_redraw) {
            draw_now_playing(ui, bar, &L, &meta, art, current_elapsed);
            needs_redraw = false;
        }

        key = nd_ui_read_keypress(ui, 1.0);
        if (key == ND_KEY_NONE) {
            /* A timeout repaints once a second, which is what advances the
             * clock and the progress bar. */
            needs_redraw = true;
            if (nd_app_should_exit())
                break;
            continue;
        }

        needs_redraw = true;
        if (key == ND_KEY_CLEAR) {
            nd_music_stop();
            result = NP_BACK;
            break;
        }
        if (key == ND_KEY_ENTER) {
            nd_music_toggle_pause();
            continue;
        }
        /* Volume. UP and DOWN are the keys the MediaWidget already binds to
         * it (lib/nd_media.c), and 2 and 8 are the same keys on a matrix
         * keypad that has no arrows.
         *
         * The key takes its step FIRST and then the full-screen element
         * opens showing the result, so one press is one step whether or not
         * you stay to make another -- rather than the first press only
         * opening a screen and doing nothing. The track keeps playing
         * throughout, which is the whole point: you adjust it by ear. */
        if (key == ND_KEY_UP || key == ND_KEY_2) {
            nd_music_set_volume(nd_music_volume() + 1);
            volume_screen(ui);
            continue;
        }
        if (key == ND_KEY_DOWN || key == ND_KEY_8) {
            nd_music_set_volume(nd_music_volume() - 1);
            volume_screen(ui);
            continue;
        }
        if (key == ND_KEY_RIGHT || key == ND_KEY_6) {
            nd_music_stop();
            result = NP_NEXT;
            break;
        }
        if (key == ND_KEY_LEFT || key == ND_KEY_4) {
            nd_music_stop();
            result = NP_PREV;
            break;
        }
    }

    nd_image_free(art);
    nd_music_meta_free(&meta);
    return result;
}

/* Plays a whole list, starting where the caller left the cursor. NOT a port:
 * the Python played one file and returned to the menu.
 *
 * A track that will not play is SKIPPED rather than ending the queue -- one
 * corrupt file in an album should cost that track and nothing else -- but
 * only forwards, so a bad first track cannot trap Left at the top. */
static void run_queue(nd_ui *ui, nd_softkey *bar, music_queue *q)
{
    while (q->pos < q->n) {
        np_result r;

        if (!nd_music_play(q->items[q->pos].path)) {
            nd_log_err(ND_LOG_MUSIC, "skipping %s", q->items[q->pos].path);
            q->pos++;
            continue;
        }
        r = run_now_playing(ui, bar, q->items[q->pos].path);
        if (r == NP_BACK)
            return;
        if (r == NP_PREV) {
            if (q->pos == 0u)
                return; /* Left at the top of the list leaves it */
            q->pos--;
            continue;
        }
        q->pos++;
        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * The browser -- NOT a port
 * ------------------------------------------------------------------ */

/* Every list screen is a slice of the library, because the build already put
 * the runs in order: an artist's albums are contiguous, an album's songs are
 * contiguous. Nothing here searches or filters. See library.c. */

/* Plays a contiguous run of library songs starting at `start + offset`. */
static void play_run(nd_ui *ui, nd_softkey *bar, size_t start, size_t count, size_t offset)
{
    music_queue q;
    size_t i;

    if (!queue_alloc(&q, count)) {
        nd_log_err(ND_LOG_MUSIC, "out of memory building the queue");
        return;
    }
    for (i = 0u; i < count; i++) {
        const nd_music_song *sg = nd_music_library_song(g_library, start + i);

        if (sg == NULL)
            break;
        q.items[i].path = sg->path;
        q.items[i].title = sg->title;
    }
    q.n = i;
    q.pos = (offset < q.n) ? offset : 0u;
    run_queue(ui, bar, &q);
    queue_free(&q);
}

/* "3  Sour Times", or just the title when the tag carried no number. The
 * number is padded to two so a ten-track album's titles line up. */
static void song_label(char *out, size_t out_sz, const nd_music_song *sg)
{
    if (sg->track > 0u)
        (void)nd_snprintf(out, out_sz, "%2u  %s", (unsigned)sg->track, sg->title);
    else
        (void)nd_strlcpy(out, sg->title, out_sz);
}

/* One album's tracks. */
static void album_songs(nd_ui *ui, nd_softkey *bar, size_t album_index)
{
    const nd_music_album *alb = nd_music_library_album(g_library, album_index);
    music_list list;
    size_t i;

    if (alb == NULL || alb->n_songs == 0u)
        return;
    if (!music_list_alloc(&list, alb->n_songs))
        return;
    for (i = 0u; i < alb->n_songs; i++) {
        const nd_music_song *sg = nd_music_library_song(g_library, alb->first_song + i);
        char label[MUSIC_LABEL_MAX];

        if (sg == NULL)
            break;
        song_label(label, sizeof label, sg);
        music_list_set(&list, ui, i, label);
    }
    list.n = i;

    for (;;) {
        nd_vlist vl;
        int32_t sel;

        nd_vlist_init(&vl, ui, alb->name, list.ptrs, list.n, ND_MUSIC_LIST_APP_ID);
        sel = nd_vlist_show(&vl);
        if (sel < 0 || nd_app_should_exit())
            break;
        /* The whole album is the queue, starting at what was chosen. */
        play_run(ui, bar, alb->first_song, alb->n_songs, (size_t)sel);
        if (nd_app_should_exit())
            break;
    }
    music_list_free(&list);
}

/* One artist's albums, plus an "All songs" row that queues the lot. */
static void artist_albums(nd_ui *ui, nd_softkey *bar, size_t artist_index)
{
    const nd_music_artist *ar = nd_music_library_artist(g_library, artist_index);
    music_list list;
    size_t i;
    size_t rows;

    if (ar == NULL || ar->n_albums == 0u)
        return;

    /* An artist with one album does not need a menu whose only choice is
     * that album. Straight through. */
    if (ar->n_albums == 1u) {
        album_songs(ui, bar, ar->first_album);
        return;
    }

    rows = (size_t)ar->n_albums + 1u;
    if (!music_list_alloc(&list, rows))
        return;
    music_list_set(&list, ui, 0u, "All songs");
    for (i = 0u; i < ar->n_albums; i++) {
        const nd_music_album *alb = nd_music_library_album(g_library, ar->first_album + i);
        char label[MUSIC_LABEL_MAX];

        if (alb == NULL)
            break;
        /* The year earns its place here: it is what the albums are ordered
         * by, and a list that is not alphabetical looks broken without it. */
        if (alb->year > 0u)
            (void)nd_snprintf(label, sizeof label, "%s (%u)", alb->name, (unsigned)alb->year);
        else
            (void)nd_strlcpy(label, alb->name, sizeof label);
        music_list_set(&list, ui, i + 1u, label);
    }
    list.n = i + 1u;

    for (;;) {
        nd_vlist vl;
        int32_t sel;

        nd_vlist_init(&vl, ui, ar->name, list.ptrs, list.n, ND_MUSIC_LIST_APP_ID);
        sel = nd_vlist_show(&vl);
        if (sel < 0 || nd_app_should_exit())
            break;
        if (sel == 0) {
            /* The artist's songs are contiguous across all their albums,
             * which is exactly what the build guarantees. */
            const nd_music_album *first = nd_music_library_album(g_library, ar->first_album);

            if (first != NULL)
                play_run(ui, bar, first->first_song, ar->n_songs, 0u);
        } else {
            album_songs(ui, bar, ar->first_album + (size_t)(sel - 1));
        }
        if (nd_app_should_exit())
            break;
    }
    music_list_free(&list);
}

static void browse_artists(nd_ui *ui, nd_softkey *bar)
{
    music_list list;
    size_t n = nd_music_library_n_artists(g_library);
    size_t i;

    if (!music_list_alloc(&list, n))
        return;
    for (i = 0u; i < n; i++)
        music_list_set(&list, ui, i, nd_music_library_artist(g_library, i)->name);
    list.n = n;

    for (;;) {
        nd_vlist vl;
        int32_t sel;

        nd_vlist_init(&vl, ui, "Artists", list.ptrs, list.n, ND_MUSIC_LIST_APP_ID);
        sel = nd_vlist_show(&vl);
        if (sel < 0 || nd_app_should_exit())
            break;
        artist_albums(ui, bar, (size_t)sel);
        if (nd_app_should_exit())
            break;
    }
    music_list_free(&list);
}

static void browse_albums(nd_ui *ui, nd_softkey *bar)
{
    music_list list;
    size_t n = nd_music_library_n_albums(g_library);
    size_t i;

    if (!music_list_alloc(&list, n))
        return;
    for (i = 0u; i < n; i++) {
        const nd_music_album *alb = nd_music_library_album(g_library, i);
        const nd_music_artist *ar = nd_music_library_artist(g_library, alb->artist);
        char label[MUSIC_LABEL_MAX];

        /* The artist is part of the row here and not in the per-artist view,
         * because two bands really do both have a "Greatest Hits" and this
         * is the one screen where they sit next to each other. */
        (void)nd_snprintf(label, sizeof label, "%s - %s", alb->name,
                          (ar != NULL) ? ar->name : "");
        music_list_set(&list, ui, i, label);
    }
    list.n = n;

    for (;;) {
        nd_vlist vl;
        int32_t sel;

        nd_vlist_init(&vl, ui, "Albums", list.ptrs, list.n, ND_MUSIC_LIST_APP_ID);
        sel = nd_vlist_show(&vl);
        if (sel < 0 || nd_app_should_exit())
            break;
        album_songs(ui, bar, (size_t)sel);
        if (nd_app_should_exit())
            break;
    }
    music_list_free(&list);
}

/* Every song, in title order. The queue is the whole library in that same
 * order, so playing on from a song found here keeps going alphabetically
 * rather than jumping into its album. */
static void browse_songs(nd_ui *ui, nd_softkey *bar)
{
    music_list list;
    size_t n = nd_music_library_n_songs(g_library);
    size_t i;

    if (!music_list_alloc(&list, n))
        return;
    for (i = 0u; i < n; i++)
        music_list_set(&list, ui, i, nd_music_library_song_by_title(g_library, i)->title);
    list.n = n;

    for (;;) {
        nd_vlist vl;
        music_queue q;
        int32_t sel;

        nd_vlist_init(&vl, ui, "Songs", list.ptrs, list.n, ND_MUSIC_LIST_APP_ID);
        sel = nd_vlist_show(&vl);
        if (sel < 0 || nd_app_should_exit())
            break;

        if (queue_alloc(&q, n)) {
            for (i = 0u; i < n; i++) {
                const nd_music_song *sg = nd_music_library_song_by_title(g_library, i);

                q.items[i].path = sg->path;
                q.items[i].title = sg->title;
            }
            q.n = n;
            q.pos = (size_t)sel;
            run_queue(ui, bar, &q);
            queue_free(&q);
        }
        if (nd_app_should_exit())
            break;
    }
    music_list_free(&list);
}

/* ------------------------------------------------------------------ *
 * Folders -- the Python's own screen, unchanged
 * ------------------------------------------------------------------ */

/* This is the list the app used to open on: basenames in nd_music_scan()'s
 * walk order, which OPEN-QUESTIONS.md MU-11 describes and test_scan pins. It
 * is kept, and kept exactly, because it is the only view that shows what is
 * actually on the card when the tags are wrong -- and because throwing away
 * a ported screen to replace it is not what "make it better" asked for. */
static void browse_folders(nd_ui *ui, nd_softkey *bar)
{
    for (;;) {
        playlist p;
        nd_vlist list;
        int32_t sel;

        if (!playlist_load(&p, nd_music_backend_now())) {
            nd_log_err(ND_LOG_MUSIC, "out of memory building the playlist");
            return;
        }
        if (p.n == 0u) {
            show_no_music(ui, bar);
            playlist_free(&p);
            return;
        }

        /* app_id=4 -- see note 1. */
        nd_vlist_init(&list, ui, "Music", p.name_ptrs, p.n, ND_MUSIC_LIST_APP_ID);
        sel = nd_vlist_show(&list);
        if (sel == ND_WIDGET_BACK) {
            playlist_free(&p);
            return;
        }

        if (sel >= 0 && (size_t)sel < p.n) {
            music_queue q;
            size_t i;

            /* The folder list is a queue too, so a directory of tracks plays
             * through in the order it is shown. */
            if (queue_alloc(&q, p.n)) {
                for (i = 0u; i < p.n; i++) {
                    q.items[i].path = p.tracks[i].path;
                    q.items[i].title = p.names[i];
                }
                q.n = p.n;
                q.pos = (size_t)sel;
                run_queue(ui, bar, &q);
                queue_free(&q);
            }
        }
        playlist_free(&p);

        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * The volume screen -- NOT a port
 * ------------------------------------------------------------------ */

/* The one volume UI. Reached from the menu so the level can be set before
 * anything plays, and from Now Playing on UP or DOWN.
 *
 * Full screen rather than an indicator in a corner: at 240x175 a ten-segment
 * scale small enough to sit beside "Now Playing" is a row of specks, and the
 * screen it would share is already carrying artwork, three lines of tags, a
 * progress bar and two timestamps. Changes apply as they are made, so the
 * track you are listening to is what you set the level against. */
static void volume_screen(nd_ui *ui)
{
    nd_softkey bar;
    int32_t screen_w = nd_ui_width(ui);
    int32_t content_bottom = nd_ui_content_bottom(ui);

    nd_softkey_init(&bar, ui, false);

    for (;;) {
        char value[16];
        int32_t key;
        int32_t w = 0;
        int32_t i;
        int32_t seg_w = 16;
        int32_t seg_h = 22;
        int32_t gap = 3;
        int32_t total = ND_MUSIC_VOLUME_MAX * (seg_w + gap) - gap;
        int32_t x0 = (screen_w - total) / 2;
        int32_t y0 = 74;

        (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);

        nd_text_size(ui->font_n, "Volume", &w, NULL);
        (void)nd_draw_text(ui->draw, (screen_w - w) / 2, 14, "Volume", ui->font_n, ND_WHITE);

        (void)nd_snprintf(value, sizeof value, "%d", (int)nd_music_volume());
        nd_text_size(ui->font_xl, value, &w, NULL);
        (void)nd_draw_text(ui->draw, (screen_w - w) / 2, 40, value, ui->font_xl, ND_WHITE);

        /* The same scale as the header bar, drawn large. White on black
         * here, because this screen has no white band to sit on. */
        for (i = 0; i < ND_MUSIC_VOLUME_MAX; i++) {
            int32_t sx = x0 + i * (seg_w + gap);
            nd_rect seg = ND_RECT(sx, y0, sx + seg_w - 1, y0 + seg_h - 1);

            if (i < nd_music_volume())
                (void)nd_draw_rect_fill(ui->draw, seg, ND_WHITE);
            else
                (void)nd_draw_rect_outline(ui->draw, seg, ND_GRAY, 1);
        }

        nd_softkey_update(&bar, "OK", true);

        key = nd_ui_wait_for_key(ui);
        if (nd_app_should_exit())
            return;
        if (key == ND_KEY_UP || key == ND_KEY_RIGHT || key == ND_KEY_2 || key == ND_KEY_6)
            nd_music_set_volume(nd_music_volume() + 1);
        else if (key == ND_KEY_DOWN || key == ND_KEY_LEFT || key == ND_KEY_8 || key == ND_KEY_4)
            nd_music_set_volume(nd_music_volume() - 1);
        else if (key == ND_KEY_CLEAR || key == ND_KEY_ENTER)
            return;
    }
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

typedef enum {
    MENU_ARTISTS = 0,
    MENU_ALBUMS,
    MENU_SONGS,
    MENU_FOLDERS,
    MENU_VOLUME,
    MENU_COUNT
} menu_id;

/* The three library rows say what they are FOR when the tags are missing,
 * rather than opening an empty screen: a card of untagged files browsed by
 * artist is one row called "Unknown Artist", and being told that up front is
 * better than finding it out three menus down. */
static void music_run(nd_ui *ui, nd_softkey *bar)
{
    for (;;) {
        const char *items[MENU_COUNT];
        char volume_label[32];
        nd_vlist menu;
        nd_softkey ok;
        int32_t sel;

        (void)nd_snprintf(volume_label, sizeof volume_label, "Volume: %d",
                          (int)nd_music_volume());

        items[MENU_ARTISTS] = "Artists";
        items[MENU_ALBUMS] = "Albums";
        items[MENU_SONGS] = "Songs";
        items[MENU_FOLDERS] = "Folders";
        items[MENU_VOLUME] = volume_label;

        /* An "OK" painted without presenting, which VerticalList's partial
         * clear then leaves standing -- the same throwaway bar Calculator
         * uses, and for the same reason. */
        nd_softkey_init(&ok, ui, false);
        nd_softkey_update(&ok, "OK", false);

        nd_vlist_init(&menu, ui, "Music", items, MENU_COUNT, ND_MUSIC_LIST_APP_ID);
        sel = nd_vlist_show(&menu);
        if (sel == ND_WIDGET_BACK || nd_app_should_exit())
            return;

        switch (sel) {
        case MENU_ARTISTS:
            if (ensure_library(ui))
                browse_artists(ui, bar);
            else
                show_no_music(ui, bar);
            break;
        case MENU_ALBUMS:
            if (ensure_library(ui))
                browse_albums(ui, bar);
            else
                show_no_music(ui, bar);
            break;
        case MENU_SONGS:
            if (ensure_library(ui))
                browse_songs(ui, bar);
            else
                show_no_music(ui, bar);
            break;
        case MENU_FOLDERS:
            browse_folders(ui, bar);
            break;
        case MENU_VOLUME:
            volume_screen(ui);
            break;
        default:
            break;
        }

        if (nd_app_should_exit())
            return;
    }
}

int app_run(nd_ui *ui)
{
    nd_softkey bar;
    char dir[ND_MUSIC_PATH_MAX];

    if (ui == NULL)
        return 1;

    /* `self.softkey = SoftKeyBar(ui)` and `self.player = _pick_player()`,
     * both in MusicPlayer.__init__ and both before music_dir() is asked. */
    nd_softkey_init(&bar, ui, false);
    nd_music_player_init(nd_music_pick_player());
    /* Before the card is even looked at, so the level shown on the menu is
     * the saved one and not the built-in default. */
    nd_music_volume_load();

    if (!nd_music_dir(dir, sizeof dir)) {
        nd_msgdialog dlg;
        nd_scroller help;

        /* Say why there is nothing to play rather than showing an empty
         * list. THIS IS golden/app-musicplayer.png. */
        nd_msgdialog_init(&dlg, ui, nd_music_no_card_message);
        nd_msgdialog_set_button(&dlg, "More");
        (void)nd_msgdialog_show(&dlg);

        nd_scroller_init(&help, ui, nd_music_no_card_help, NULL, NULL);
        nd_scroller_show(&help);
        return 0;
    }

    music_run(ui, &bar);

    /* `finally: app.stop()`. */
    nd_music_stop();
    nd_music_library_free(g_library);
    g_library = NULL;
    g_library_tried = false;
    return 0;
}

/* The sound card is the whole reason nd_app.h makes this mandatory: if aplay
 * or mpv is still holding ALSA when the modem thread signals an incoming
 * call, the phone rings silently. */
void app_shutdown(void)
{
    nd_music_stop();
}
