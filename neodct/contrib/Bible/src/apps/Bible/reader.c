/* reader.c -- the chapter view, and the number-entry screen it shares with
 * the chapter picker.
 *
 * ============ WHY THIS IS NOT A DetailPage OR A TextScroller ============
 *
 * Both existing widgets lay a whole document out up front into a bounded
 * array -- ND_DETAIL_MAX_BLOCKS is 256 and ND_SCROLLER_MAX_LINES is 128 --
 * and Psalm 119 wraps to about seven hundred lines at 14 px. Raising either
 * cap would change a widget the rest of the phone depends on, for one app.
 *
 * So the chapter is laid out here, once, into two buffers sized from the
 * pack's own max_raw:
 *
 *     arena   the wrapped lines, back to back, NUL-separated
 *     lines   (offset, length, verse number) per line
 *
 * For the shipped pack that is a 40 KB arena and 24 KB of index, both
 * malloc'd on entry to bible_read() and freed on the way out. Scrolling then
 * costs nothing but a redraw: no rewrap, no re-inflate, and no allocation
 * anywhere in the key loop.
 *
 * ============ THE VERSE NUMBER IS PART OF THE LINE ============
 *
 * "16 For God so loved..." is wrapped as one string, and the leading digits
 * are remembered as num_len so the draw can put them in grey and the rest in
 * white. Wrapping the number separately would have meant a narrower first
 * line and a second wrap width to keep straight; measuring the number's
 * ADVANCE (not its ink, which is narrower and would leave the text creeping
 * left) puts the white text back exactly where the wrapper thought it was.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "bibleapp.h"

/* ---- layout ------------------------------------------------------ */

#define R_MARGIN_X    6
#define R_BAR_X       235
#define R_TEXT_W      224 /* R_BAR_X - 5 - R_MARGIN_X */
#define R_HEADER_Y    2
#define R_DIVIDER_Y   20
#define R_BODY_TOP    25
#define R_LINE_H      16
#define R_VISIBLE     7 /* (145 - R_BODY_TOP) / R_LINE_H, floored */
#define R_BODY_BOTTOM (R_BODY_TOP + R_VISIBLE * R_LINE_H)

/* A ceiling on the line index, not an expectation: Psalm 119 is about 700
 * lines and 4 Maccabees 15 is the longest chapter in the shipped pack. Gen Z
 * mode expands the text, so this is roughly twice the worst case. */
#define R_MAX_LINES 2048

typedef struct {
    uint32_t off;   /* into the arena */
    uint16_t len;   /* bytes, excluding the NUL */
    uint16_t verse; /* 1-based; the number this line belongs to */
    uint8_t num_len;/* leading bytes that are the verse number and its space */
} r_line;

typedef struct {
    bible_app *a;
    nd_ui *ui;
    nd_softkey softkey;

    char *arena;
    size_t arena_sz;
    size_t arena_used;

    r_line *lines;
    size_t n_lines;

    size_t book;
    size_t chapter;
    size_t top; /* index of the first line on screen */
} reader;

/* ------------------------------------------------------------------ *
 * bible_verse_text
 * ------------------------------------------------------------------ */

const char *bible_verse_text(bible_app *a, size_t book, size_t chapter, size_t verse, char *buf,
                             size_t buf_sz)
{
    const char *raw = nd_bible_verse(a->b, verse);

    if (!a->genz)
        return raw;
    (void)nd_genz(buf, buf_sz, raw, nd_genz_seed(book, chapter, verse));
    return buf;
}

/* ------------------------------------------------------------------ *
 * Laying a chapter out
 * ------------------------------------------------------------------ */

static bool arena_push(reader *r, const char *s, size_t len, uint16_t verse, uint8_t num_len)
{
    if (r->n_lines >= R_MAX_LINES)
        return false;
    if (r->arena_used + len + 1u > r->arena_sz)
        return false;

    memcpy(r->arena + r->arena_used, s, len);
    r->arena[r->arena_used + len] = '\0';
    r->lines[r->n_lines].off = (uint32_t)r->arena_used;
    r->lines[r->n_lines].len = (uint16_t)len;
    r->lines[r->n_lines].verse = verse;
    r->lines[r->n_lines].num_len = num_len;
    r->n_lines++;
    r->arena_used += len + 1u;
    return true;
}

/* Wraps every verse of the loaded chapter into the arena. Returns false only
 * when a buffer filled, which truncates the chapter rather than losing it --
 * the reader would rather show most of 4 Maccabees than refuse to open it. */
static bool layout(reader *r)
{
    char storage[32][ND_TEXT_LINE_MAX];
    char joined[BIBLE_VERSE_MAX];
    char rendered[BIBLE_VERSE_MAX];
    nd_lines wrapped;
    size_t n_verses = nd_bible_verse_count(r->a->b);
    size_t v;

    r->arena_used = 0u;
    r->n_lines = 0u;

    for (v = 1u; v <= n_verses; v++) {
        const char *text = bible_verse_text(r->a, r->book, r->chapter, v, rendered, sizeof rendered);
        int num_len;
        size_t i;

        if (text[0] == '\0')
            continue; /* a gap the packer filled; nothing to show */

        num_len = snprintf(joined, sizeof joined, "%u ", (unsigned)v);
        if (num_len < 0)
            continue;
        (void)nd_strlcat(joined, text, sizeof joined);

        nd_lines_init(&wrapped, storage, 32u);
        nd_text_wrap(&wrapped, joined, r->ui->font_s, R_TEXT_W);

        for (i = 0u; i < wrapped.n; i++) {
            const char *line = nd_lines_at(&wrapped, i);
            /* Only the first line of a verse carries the number, and only
             * when the wrapper did not split between the digits and the word
             * after them -- which it cannot, since it breaks on spaces and
             * the number plus one word always fits. */
            uint8_t nl = (i == 0u) ? (uint8_t)num_len : (uint8_t)0;

            if (!arena_push(r, line, strlen(line), (uint16_t)v, nl))
                return false;
        }
    }
    return true;
}

static nd_err load_chapter(reader *r, size_t book, size_t chapter)
{
    nd_err rc = nd_bible_load(r->a->b, book, chapter);

    if (rc != ND_OK)
        return rc;
    r->book = book;
    r->chapter = chapter;
    (void)layout(r);
    r->top = 0u;
    return ND_OK;
}

/* Scroll so that `verse` is the first thing on screen. A verse the layout
 * skipped (an empty one) has no line, so the nearest following verse wins. */
static void scroll_to_verse(reader *r, size_t verse)
{
    size_t i;

    if (verse == 0u)
        return;
    for (i = 0u; i < r->n_lines; i++) {
        if (r->lines[i].verse >= verse) {
            r->top = i;
            return;
        }
    }
}

static size_t max_top(const reader *r)
{
    return (r->n_lines > (size_t)R_VISIBLE) ? r->n_lines - (size_t)R_VISIBLE : 0u;
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

static void draw_scrollbar(reader *r)
{
    nd_draw *d = r->ui->draw;
    int32_t track_top = R_BODY_TOP;
    int32_t track_bottom = R_BODY_BOTTOM - 1;
    size_t span = max_top(r);
    double notch;

    (void)nd_draw_line(d, R_BAR_X, track_top, R_BAR_X, track_bottom, ND_GRAY, 1);
    if (span == 0u) {
        notch = (double)track_top;
    } else {
        /* Same shape as VerticalList's, and the same nd_trunc32 at the end:
         * Pillow truncates a float coordinate and a round() here would put
         * the notch one pixel off the widget beside it. */
        double step = (double)(track_bottom - track_top) / (double)span;

        notch = (double)track_top + ((double)r->top * step);
    }
    (void)nd_draw_rect_fill(d, ND_RECT(R_BAR_X - 2, nd_trunc32(notch - 3.0), R_BAR_X + 2,
                                       nd_trunc32(notch + 3.0)),
                            ND_WHITE);
}

static void draw(reader *r)
{
    nd_ui *ui = r->ui;
    nd_draw *d = ui->draw;
    char title[64];
    char badge[16];
    int32_t badge_w = 0;
    size_t i;

    (void)nd_draw_rect_fill(d, ND_RECT(0, 0, ND_UI_W - 1, ui->content_bottom), ND_BLACK);

    /* The mode badge is drawn first because the title is then fitted into
     * whatever is left, exactly as VerticalList fits its title around the
     * breadcrumb. */
    (void)nd_strlcpy(badge, r->a->genz ? "GEN Z" : nd_bible_translation(r->a->b), sizeof badge);
    nd_ui_text_size(ui, badge, ui->font_s, &badge_w, NULL);
    (void)nd_draw_text(d, ND_UI_W - 5 - badge_w, R_HEADER_Y, badge, ui->font_s, ND_GRAY);

    bible_format_ref(r->a->b, r->book, r->chapter + 1u, 0u, title, sizeof title);
    {
        char fitted[64];

        (void)nd_text_fit(fitted, sizeof fitted, title, ui->font_n,
                          ND_UI_W - R_MARGIN_X - badge_w - 12);
        (void)nd_draw_text(d, R_MARGIN_X, R_HEADER_Y - 2, fitted, ui->font_n, ND_WHITE);
    }
    (void)nd_draw_line(d, 0, R_DIVIDER_Y, ND_UI_W - 1, R_DIVIDER_Y, ND_GRAY, 1);

    for (i = 0u; i < (size_t)R_VISIBLE; i++) {
        size_t idx = r->top + i;
        const r_line *ln;
        const char *text;
        int32_t y = R_BODY_TOP + (int32_t)i * R_LINE_H;

        if (idx >= r->n_lines)
            break;
        ln = &r->lines[idx];
        text = r->arena + ln->off;

        if (ln->num_len > 0u) {
            char num[8];
            int32_t advance;

            (void)nd_strlcpy(num, text, (size_t)ln->num_len + 1u);
            (void)nd_draw_text(d, R_MARGIN_X, y, num, ui->font_s, ND_GRAY);
            advance = nd_text_advance(ui->font_s, num);
            (void)nd_draw_text(d, R_MARGIN_X + advance, y, text + ln->num_len, ui->font_s,
                               ND_WHITE);
        } else {
            (void)nd_draw_text(d, R_MARGIN_X, y, text, ui->font_s, ND_WHITE);
        }
    }

    if (r->n_lines == 0u) {
        (void)nd_draw_text(d, R_MARGIN_X, R_BODY_TOP + 20, "(this chapter is empty)", ui->font_s,
                           ND_GRAY);
    }

    draw_scrollbar(r);
    nd_softkey_update(&r->softkey, "Menu", true);
}

/* ------------------------------------------------------------------ *
 * bible_pick_number
 * ------------------------------------------------------------------ */

int32_t bible_pick_number(nd_ui *ui, const char *title, int32_t lo, int32_t hi, int32_t cur)
{
    nd_softkey bar;
    char value[8];
    char range[32];
    int32_t typed = -1; /* >= 0 once a digit has been pressed */

    if (lo > hi)
        return -1;
    cur = nd_clamp32(cur, lo, hi);
    nd_softkey_init(&bar, ui, false);
    (void)nd_snprintf(range, sizeof range, "of %d", hi);

    for (;;) {
        int32_t key;
        char fitted[40];
        int32_t w = 0;

        (void)nd_snprintf(value, sizeof value, "%d", (typed >= 0) ? typed : cur);

        (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, ND_UI_W - 1, ui->content_bottom),
                                ND_BLACK);
        (void)nd_text_fit(fitted, sizeof fitted, title, ui->font_n, ND_UI_W - 20);
        nd_ui_text_size(ui, fitted, ui->font_n, &w, NULL);
        (void)nd_draw_text(ui->draw, (ND_UI_W - w) / 2, 14, fitted, ui->font_n, ND_WHITE);

        nd_ui_text_size(ui, value, ui->font_xl, &w, NULL);
        (void)nd_draw_text(ui->draw, (ND_UI_W - w) / 2, 55, value, ui->font_xl, ND_WHITE);

        nd_ui_text_size(ui, range, ui->font_s, &w, NULL);
        (void)nd_draw_text(ui->draw, (ND_UI_W - w) / 2, 100, range, ui->font_s, ND_GRAY);

        nd_softkey_update(&bar, "OK", true);

        key = nd_ui_wait_for_key(ui);
        if (nd_app_should_exit())
            return -1;

        if (nd_key_is_digit(key)) {
            int32_t digit = (int32_t)(nd_key_digit_char(key) - '0');
            int32_t next = ((typed >= 0) ? typed : 0) * 10 + digit;

            /* Typing past the end restarts from the digit just pressed, so
             * "1", "5", "0" on a 150-chapter book reaches 150 and a fourth
             * digit starts over rather than sticking at the top. */
            typed = (next <= hi) ? next : digit;
            if (typed > hi)
                typed = hi;
            continue;
        }
        if (key == ND_KEY_UP || key == ND_KEY_RIGHT) {
            cur = ((typed >= 0) ? typed : cur) + 1;
            if (cur > hi)
                cur = lo;
            typed = -1;
            continue;
        }
        if (key == ND_KEY_DOWN || key == ND_KEY_LEFT) {
            cur = ((typed >= 0) ? typed : cur) - 1;
            if (cur < lo)
                cur = hi;
            typed = -1;
            continue;
        }
        if (key == ND_KEY_ENTER) {
            int32_t chosen = (typed >= 0) ? typed : cur;

            if (chosen < lo)
                continue; /* "0" typed on a 1-based list is not an answer */
            return nd_clamp32(chosen, lo, hi);
        }
        if (key == ND_KEY_CLEAR) {
            if (typed > 0) {
                typed /= 10;
                if (typed == 0)
                    typed = -1;
                continue;
            }
            if (typed == 0) {
                typed = -1;
                continue;
            }
            return -1;
        }
    }
}

/* ------------------------------------------------------------------ *
 * The options menu
 * ------------------------------------------------------------------ */

typedef enum {
    R_OPT_BOOKMARK = 0,
    R_OPT_GOTO_VERSE,
    R_OPT_NEXT,
    R_OPT_PREV,
    R_OPT_MODE,
    R_OPT_COUNT
} r_option;

static void show_note(nd_ui *ui, const char *message)
{
    nd_msgdialog dlg;

    nd_msgdialog_init(&dlg, ui, message);
    nd_msgdialog_set_button(&dlg, "OK");
    (void)nd_msgdialog_show(&dlg);
}

/* Returns true when the caller must re-lay the chapter out (the mode
 * changed); the chapter moves are applied here. */
static bool options_menu(reader *r)
{
    const char *items[R_OPT_COUNT];
    char mode_label[40];
    nd_vlist menu;
    nd_softkey bar;
    int32_t choice;
    size_t n_chapters = nd_bible_chapter_count(r->a->b, r->book);

    (void)nd_snprintf(mode_label, sizeof mode_label, "Read in %s",
                      r->a->genz ? nd_bible_translation(r->a->b) : "GEN Z");

    items[R_OPT_BOOKMARK] = "Bookmark this";
    items[R_OPT_GOTO_VERSE] = "Go to verse...";
    items[R_OPT_NEXT] = "Next chapter";
    items[R_OPT_PREV] = "Previous chapter";
    items[R_OPT_MODE] = mode_label;

    nd_softkey_init(&bar, r->ui, false);
    nd_softkey_update(&bar, "OK", false);
    nd_vlist_init(&menu, r->ui, "Options", items, R_OPT_COUNT, BIBLE_APP_ID);
    choice = nd_vlist_show(&menu);

    switch (choice) {
    case R_OPT_BOOKMARK: {
        size_t verse = (r->n_lines > 0u) ? r->lines[r->top].verse : 1u;

        if (bible_bookmark_add(r->a, r->book, r->chapter, verse))
            show_note(r->ui, "Bookmark saved.");
        else
            show_note(r->ui, "Bookmark list is full.");
        return false;
    }
    case R_OPT_GOTO_VERSE: {
        size_t n = nd_bible_verse_count(r->a->b);
        int32_t v;

        if (n == 0u)
            return false;
        v = bible_pick_number(r->ui, "Verse", 1, (int32_t)n,
                              (r->n_lines > 0u) ? (int32_t)r->lines[r->top].verse : 1);
        if (v > 0)
            scroll_to_verse(r, (size_t)v);
        return false;
    }
    case R_OPT_NEXT:
        if (r->chapter + 1u < n_chapters)
            (void)load_chapter(r, r->book, r->chapter + 1u);
        return false;
    case R_OPT_PREV:
        if (r->chapter > 0u)
            (void)load_chapter(r, r->book, r->chapter - 1u);
        return false;
    case R_OPT_MODE:
        r->a->genz = !r->a->genz;
        r->a->state_dirty = true;
        return true;
    default:
        return false;
    }
}

/* ------------------------------------------------------------------ *
 * bible_read
 * ------------------------------------------------------------------ */

/* Re-lays the current chapter and keeps the reader looking at roughly the
 * same verse, which is what makes toggling Gen Z mid-chapter feel like a
 * change of voice rather than a jump. */
static void relayout_keeping_place(reader *r)
{
    size_t verse = (r->n_lines > 0u) ? r->lines[r->top].verse : 0u;

    (void)layout(r);
    r->top = 0u;
    scroll_to_verse(r, verse);
}

static void step_chapter(reader *r, int delta)
{
    size_t n = nd_bible_chapter_count(r->a->b, r->book);

    if (delta > 0 && r->chapter + 1u < n)
        (void)load_chapter(r, r->book, r->chapter + 1u);
    else if (delta < 0 && r->chapter > 0u)
        (void)load_chapter(r, r->book, r->chapter - 1u);
}

void bible_read(bible_app *a, size_t book, size_t chapter, size_t verse)
{
    reader r;
    size_t max_raw = nd_bible_max_raw(a->b);

    memset(&r, 0, sizeof r);
    r.a = a;
    r.ui = a->ui;
    nd_softkey_init(&r.softkey, a->ui, false);

    /* Twice the largest chapter, plus room for a verse number on every line.
     * Gen Z mode is the reason for the factor: it lengthens the text, and a
     * layout that silently truncated Genesis 1 would look like a bug in the
     * pack rather than in this number. */
    r.arena_sz = max_raw * 3u + 8192u;
    r.arena = malloc(r.arena_sz);
    r.lines = malloc((size_t)R_MAX_LINES * sizeof *r.lines);
    if (r.arena == NULL || r.lines == NULL) {
        free(r.arena);
        free(r.lines);
        show_note(a->ui, "Not enough memory to open that chapter.");
        return;
    }

    if (load_chapter(&r, book, chapter) != ND_OK) {
        free(r.arena);
        free(r.lines);
        show_note(a->ui, "That chapter could not be read.");
        return;
    }
    scroll_to_verse(&r, verse);
    draw(&r);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(a->ui);
        size_t before_top = r.top;
        size_t before_chapter = r.chapter;

        if (nd_app_should_exit())
            break;

        if (key == ND_KEY_DOWN || key == ND_KEY_8) {
            if (r.top < max_top(&r))
                r.top++;
        } else if (key == ND_KEY_UP || key == ND_KEY_2) {
            if (r.top > 0u)
                r.top--;
        } else if (key == ND_KEY_0) {
            /* A page, minus one line of overlap, which is what stops a
             * sentence that straddles the fold from being read twice or not
             * at all. */
            size_t page = (size_t)R_VISIBLE - 1u;

            r.top = (r.top + page < max_top(&r)) ? r.top + page : max_top(&r);
        } else if (key == ND_KEY_RIGHT || key == ND_KEY_6 || key == ND_KEY_HASH) {
            step_chapter(&r, +1);
        } else if (key == ND_KEY_LEFT || key == ND_KEY_4 || key == ND_KEY_STAR) {
            step_chapter(&r, -1);
        } else if (key == ND_KEY_5) {
            size_t n = nd_bible_verse_count(a->b);
            int32_t v = (n == 0u) ? -1
                                  : bible_pick_number(a->ui, "Verse", 1, (int32_t)n,
                                                      (r.n_lines > 0u)
                                                          ? (int32_t)r.lines[r.top].verse
                                                          : 1);

            if (v > 0)
                scroll_to_verse(&r, (size_t)v);
            draw(&r);
            continue;
        } else if (key == ND_KEY_ENTER) {
            if (options_menu(&r))
                relayout_keeping_place(&r);
            draw(&r);
            continue;
        } else if (key == ND_KEY_CLEAR) {
            break;
        } else {
            continue;
        }

        if (r.top != before_top || r.chapter != before_chapter)
            draw(&r);
    }

    bible_remember(a, r.book, r.chapter, (r.n_lines > 0u) ? r.lines[r.top].verse : 1u);
    free(r.arena);
    free(r.lines);
}
