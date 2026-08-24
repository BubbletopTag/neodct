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

static void run_now_playing(nd_ui *ui, nd_softkey *bar, const char *filepath)
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

    /* 1. The loading card. Note it clears the WHOLE screen including the
     *    softkey strip, and presents directly rather than through the bar. */
    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, screen_h), ND_BLACK);
    nd_text_size(ui->font_n, "Loading...", &lw, &lh);
    (void)nd_draw_text(ui->draw, (screen_w - lw) / 2, nd_max32(10, (content_bottom - lh) / 2),
                       "Loading...", ui->font_n, ND_WHITE);
    (void)nd_ui_present(ui);

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
            break;
        }
        if (key == ND_KEY_ENTER)
            nd_music_toggle_pause();
    }

    nd_image_free(art);
    nd_music_meta_free(&meta);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

static void music_run(nd_ui *ui, nd_softkey *bar)
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
            char path[ND_MUSIC_PATH_MAX];

            (void)nd_strlcpy(path, p.tracks[sel].path, sizeof path);
            playlist_free(&p);
            if (nd_music_play(path))
                run_now_playing(ui, bar, path);
        } else {
            playlist_free(&p);
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
    return 0;
}

/* The sound card is the whole reason nd_app.h makes this mandatory: if aplay
 * or mpv is still holding ALSA when the modem thread signals an incoming
 * call, the phone rings silently. */
void app_shutdown(void)
{
    nd_music_stop();
}
