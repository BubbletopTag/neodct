/* nd_pagedlist.c -- PagedList: one item per screen, in 24 px type, the way the
 * 5190's main menus worked.
 *
 * Used by the Call Log (three times), Messages and Tones.
 *
 * ============ THIS ONE CLEARS THE WHOLE SCREEN ============
 *
 * Unlike VerticalList, draw() clears rows 0..175 and then paints the softkey
 * itself. A caller that sets a softkey before calling this loses it, and that
 * is correct -- PagedList owns the whole frame.
 *
 * ============ THE CENTRING QUIRK IS LOAD-BEARING ============
 *
 * Item text is centred inside max_w = bar_x - 12 = 223, not inside the 240 px
 * screen, so every item sits about eight pixels left of the true centre. It
 * looks like a bug and it is what the Call Log has always looked like. Port
 * it; the golden frame will catch a "fix".
 *
 * ============ THE FOURTH WRAPPER ============
 *
 * nd_pagedlist_wrap() is not any of the three in nd_text.h. It splits on ANY
 * whitespace with no empty tokens, so newlines and runs of spaces collapse
 * entirely, and it appends "..." to a hard-trimmed over-long word only when
 * that word is NOT the last one. Both quirks are visible on the Call Log and
 * both are pinned by unit tests.
 *
 * ============ HOLD-TO-REPEAT ============
 *
 * As in nd_vlist: nd_input synthesises repeat presses for the arrows from its
 * own held state, so holding Up or Down pages through the list. This loop's
 * only job is to treat a synthesised press exactly like a real one, which it
 * does by not asking where the key came from. Paging wraps with a modulo, so a
 * held arrow cycles rather than stopping -- the same as tapping, only faster.
 */

#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#define PAGEDLIST_WRAP_LINES 2 /* max_lines every shipped caller asks for */

static int32_t floor_div2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

static int32_t text_w(const nd_font *f, const char *s)
{
    int32_t w = 0;

    nd_text_size(f, s, &w, NULL);
    return w;
}

/* One byte offset back, over a whole UTF-8 sequence. Python's trimmed[:-1]
 * removes a CHARACTER; doing it a byte at a time would leave a truncated
 * sequence in the middle of the measurement. */
static size_t utf8_back(const char *s, size_t len)
{
    while (len > 0u) {
        len--;
        if (((unsigned char)s[len] & 0xC0u) != 0x80u)
            break;
    }
    return len;
}

/* ------------------------------------------------------------------ *
 * _wrap_to_lines
 * ------------------------------------------------------------------ */

/* text.split(): the next run of non-whitespace, empty tokens already gone.
 * Python's str.split() with no argument treats space, tab, newline, carriage
 * return, vertical tab and form feed as separators. */
static bool is_py_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

static bool next_token(const char **p, const char **start, size_t *len)
{
    const char *s = *p;
    const char *q;

    while (*s != '\0' && is_py_space(*s))
        s++;
    if (*s == '\0')
        return false;
    q = s;
    while (*q != '\0' && !is_py_space(*q))
        q++;
    *start = s;
    *len = (size_t)(q - s);
    *p = q;
    return true;
}

/* trimmed + "..." fits? trimmed is the first `len` bytes of `word`. */
static bool trimmed_fits(const nd_font *f, const char *word, size_t len, int32_t max_w,
                         char *scratch, size_t scratch_sz)
{
    if (len + 4u > scratch_sz)
        return false;
    memcpy(scratch, word, len);
    memcpy(scratch + len, "...", 4u);
    return text_w(f, scratch) <= max_w;
}

void nd_pagedlist_wrap(nd_lines *out, const char *text, const nd_font *f, int32_t max_w,
                       size_t max_lines)
{
    char cur[ND_TEXT_LINE_MAX];
    char cand[ND_TEXT_LINE_MAX];
    const char *words[64];
    size_t wlens[64];
    size_t n_words = 0u;
    size_t i;
    const char *p;
    const char *ws;
    size_t wl;
    size_t cur_len = 0u;

    if (out == NULL)
        return;
    nd_lines_clear(out);
    if (max_lines > out->cap)
        max_lines = out->cap;

    cur[0] = '\0';

    p = (text != NULL) ? text : "";
    while (n_words < ND_ARRAY_LEN(words) && next_token(&p, &ws, &wl)) {
        words[n_words] = ws;
        wlens[n_words] = wl;
        n_words++;
    }

    if (n_words == 0u) {
        /* "if not words: return [''}" -- an empty name still occupies a line,
         * which is what keeps the vertical centring stable. */
        (void)nd_lines_push(out, "");
        return;
    }

    i = 0u;
    while (i < n_words && out->n < max_lines) {
        size_t cand_len;

        /* candidate = (cur + " " + w).strip() if cur else w */
        if (cur_len > 0u) {
            cand_len = cur_len + 1u + wlens[i];
            if (cand_len + 1u > sizeof cand)
                cand_len = 0u; /* cannot even hold it: treat as "does not fit" */
        } else {
            cand_len = wlens[i];
            if (cand_len + 1u > sizeof cand)
                cand_len = 0u;
        }

        if (cand_len > 0u) {
            if (cur_len > 0u) {
                memcpy(cand, cur, cur_len);
                cand[cur_len] = ' ';
                memcpy(cand + cur_len + 1u, words[i], wlens[i]);
            } else {
                memcpy(cand, words[i], wlens[i]);
            }
            cand[cand_len] = '\0';

            if (text_w(f, cand) <= max_w) {
                memcpy(cur, cand, cand_len + 1u);
                cur_len = cand_len;
                i++;
                continue;
            }
        }

        if (cur_len > 0u) {
            (void)nd_lines_push(out, cur);
            cur[0] = '\0';
            cur_len = 0u;
            continue;
        }

        /* A single word too long for the line: hard-trim it a character at a
         * time until "word..." fits. */
        {
            size_t trimmed = wlens[i];

            while (trimmed > 0u && !trimmed_fits(f, words[i], trimmed, max_w, cand, sizeof cand))
                trimmed = utf8_back(words[i], trimmed);

            if (trimmed > 0u) {
                memcpy(cand, words[i], trimmed);
                cand[trimmed] = '\0';
                /* THE QUIRK: the "..." goes on only when this is not the last
                 * word. The last word is simply cut. */
                if (i < n_words - 1u)
                    memcpy(cand + trimmed, "...", 4u);
                (void)nd_lines_push(out, cand);
            } else {
                (void)nd_lines_push(out, "...");
            }
            i++;
        }
    }

    if (out->n < max_lines && cur_len > 0u)
        (void)nd_lines_push(out, cur);

    if (i < n_words) {
        /* Words left over: mark the last line as truncated. Python indexes
         * lines[-1] here and would raise IndexError on an empty list; the loop
         * above always appends before advancing i, so it cannot happen. The
         * guard below keeps that true rather than changing when it fires. */
        char last[ND_TEXT_LINE_MAX];
        size_t len;

        if (out->n == 0u) {
            nd_log_err("UI", "PagedList wrap: leftover words with no lines");
            return;
        }
        (void)nd_strlcpy(last, out->buf[out->n - 1u], sizeof last);
        len = strlen(last);
        if (len >= 3u && strcmp(last + (len - 3u), "...") == 0)
            return;

        while (len > 0u && !trimmed_fits(f, last, len, max_w, cand, sizeof cand))
            len = utf8_back(last, len);

        if (len > 0u) {
            memcpy(cand, last, len);
            memcpy(cand + len, "...", 4u);
        } else {
            (void)nd_strlcpy(cand, "...", sizeof cand);
        }
        (void)nd_strlcpy(out->buf[out->n - 1u], cand, ND_TEXT_LINE_MAX);
    }
}

/* ------------------------------------------------------------------ *
 * The widget
 * ------------------------------------------------------------------ */

void nd_pagedlist_init(nd_pagedlist *p, nd_ui *ui, const char *title, const char *const *items,
                       size_t n_items, const char *root_id, bool show_select_hint)
{
    if (p == NULL)
        return;

    p->ui = ui;
    p->title = title;
    p->items = items;
    p->values = NULL;
    p->n_items = (items != NULL) ? n_items : 0u;
    p->selected_index = 0u;

    nd_header_init(&p->header, ui, root_id);
    /* Opaque, always. The Python builds SoftKeyBar(ui) here and the hasattr
     * test can only be false for the core's own bar, which is built before any
     * PagedList can exist. */
    nd_softkey_init(&p->softkey, ui, false);
    p->show_select_hint = show_select_hint;

    p->content_top = nd_ui_header_divider_y(ui) + 8;
    p->content_bottom = nd_ui_content_bottom(ui) - 10;
    p->bar_x = nd_ui_width(ui) - 5;
}

/* Between the name's last line and the value. Wide enough that the two read
 * as a pair rather than as one wrapped sentence. */
#define PAGEDLIST_VALUE_GAP 10

static const char *item_name(const nd_pagedlist *p, size_t idx)
{
    if (p->n_items == 0u || idx >= p->n_items)
        return "";
    return (p->items[idx] != NULL) ? p->items[idx] : "";
}

/* "" for every row unless a caller supplied values, which is what keeps the
 * three lists that predate this unchanged to the pixel. */
static const char *item_value(const nd_pagedlist *p, size_t idx)
{
    if (p->values == NULL || p->n_items == 0u || idx >= p->n_items)
        return "";
    return (p->values[idx] != NULL) ? p->values[idx] : "";
}

void nd_pagedlist_set_values(nd_pagedlist *p, const char *const *values)
{
    if (p == NULL)
        return;
    p->values = values;
}

void nd_pagedlist_draw(nd_pagedlist *p)
{
    nd_ui *ui;
    nd_draw *d;
    int32_t screen_w;
    int32_t header_y;
    int32_t max_w;
    int32_t line_h = 0;
    int32_t value_h = 0;
    const char *value;
    int32_t total_h;
    int32_t y0;
    int32_t track_top;
    int32_t track_bottom;
    char storage[PAGEDLIST_WRAP_LINES][ND_TEXT_LINE_MAX];
    nd_lines lines;
    size_t li;
    double notch_y;

    if (p == NULL || p->ui == NULL || p->ui->draw == NULL)
        return;

    ui = p->ui;
    d = ui->draw;
    screen_w = nd_ui_width(ui);
    header_y = nd_ui_header_divider_y(ui);

    /* Full-screen clear: this widget owns the softkey strip too. */
    nd_ui_paint_chrome_full(ui);
    (void)nd_draw_text(d, 5, 5, (p->title != NULL) ? p->title : "", ui->font_xl, ND_WHITE);
    (void)nd_draw_line(d, 0, header_y, screen_w, header_y, ND_WHITE, 1);

    if (p->n_items == 0u) {
        int32_t w = 0;
        int32_t h = 0;
        int32_t y;

        nd_header_draw(&p->header, -1); /* the root id alone, no "-n" */
        nd_text_size(ui->font_n, "No Items", &w, &h);
        y = p->content_top + nd_max32(0, ((p->content_bottom - p->content_top) - h) / 2);
        (void)nd_draw_text(d, floor_div2(screen_w - w), y, "No Items", ui->font_n, ND_WHITE);
        if (p->show_select_hint)
            nd_softkey_update(&p->softkey, NULL, false);
        (void)nd_ui_present(ui);
        return;
    }

    nd_header_draw(&p->header, (int32_t)p->selected_index + 1);

    max_w = nd_max32(20, p->bar_x - 12);
    nd_lines_init(&lines, storage, PAGEDLIST_WRAP_LINES);
    nd_pagedlist_wrap(&lines, item_name(p, p->selected_index), ui->font_xl, max_w,
                      PAGEDLIST_WRAP_LINES);

    /* line_h is the ink height of "Ag" -- a fixed probe string, so unlike
     * VerticalList the row pitch does not move with the item's own letters. */
    nd_text_size(ui->font_xl, "Ag", NULL, &line_h);
    total_h = (int32_t)lines.n * (line_h + 6) - 6;

    /* The value joins the block BEFORE it is centred, so name and value move
     * together. Probed with "Ag" for the same reason line_h is: a value of
     * "Off" and one of "11:54 am" must not sit at different heights. */
    value = item_value(p, p->selected_index);
    if (value[0] != '\0') {
        nd_text_size(ui->font_n, "Ag", NULL, &value_h);
        value_h += PAGEDLIST_VALUE_GAP;
        total_h += value_h;
    }

    y0 = p->content_top + nd_max32(0, ((p->content_bottom - p->content_top) - total_h) / 2);

    for (li = 0u; li < lines.n; li++) {
        const char *line = nd_lines_at(&lines, li);
        int32_t w = 0;
        int32_t x;

        nd_text_size(ui->font_xl, line, &w, NULL);
        /* Centred inside max_w, NOT inside screen_w. See the header comment. */
        x = nd_max32(5, floor_div2(max_w - w));
        (void)nd_draw_text(d, x, y0 + (int32_t)li * (line_h + 6), line, ui->font_xl, ND_WHITE);
    }

    if (value[0] != '\0') {
        int32_t w = 0;
        int32_t x;
        int32_t y = y0 + (int32_t)lines.n * (line_h + 6) - 6 + PAGEDLIST_VALUE_GAP;

        nd_text_size(ui->font_n, value, &w, NULL);
        x = nd_max32(5, floor_div2(max_w - w));
        (void)nd_draw_text(d, x, y, value, ui->font_n, ND_WHITE);
    }

    /* Scrollbar: white, width 2, so columns bar_x and bar_x + 1. */
    track_top = p->content_top;
    track_bottom = nd_max32(track_top, p->content_bottom);
    (void)nd_draw_line(d, p->bar_x, track_top, p->bar_x, track_bottom, ND_WHITE, 2);

    if (p->n_items > 1u) {
        double step = (double)(track_bottom - track_top) / (double)(p->n_items - 1u);
        notch_y = (double)track_top + ((double)p->selected_index * step);
    } else {
        notch_y = (double)track_top;
    }
    (void)nd_draw_rect_fill(
        d,
        ND_RECT(p->bar_x - 4, nd_trunc32(notch_y - 3.0), p->bar_x + 2, nd_trunc32(notch_y + 3.0)),
        ND_WHITE);

    if (p->show_select_hint)
        nd_softkey_update(&p->softkey, "Select", false);

    (void)nd_ui_present(ui);
}

int32_t nd_pagedlist_show(nd_pagedlist *p)
{
    if (p == NULL)
        return ND_WIDGET_BACK;

    /* Input flush before the first draw, so a key pressed on the screen this
     * one replaced does not immediately page it.
     *
     * The Python polls select() with a 0.01 s timeout and stops on the first
     * idle poll -- NOT a bare non-blocking drain, and nd_input.h is explicit
     * that PagedList's 0.01 and MessageDialog's 0.0 are to be kept apart. The
     * ten milliseconds are what catch a record that was already on its way. */
    if (p->ui != NULL && p->ui->input != NULL) {
        nd_key_event ev;
        int guard;

        for (guard = 0; guard < 256; guard++) {
            if (!nd_input_read_event(p->ui->input, 0.01, &ev))
                break;
        }
    }

    if (p->selected_index >= p->n_items)
        p->selected_index = 0u;

    nd_pagedlist_draw(p);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(p->ui);

        if (key == ND_KEY_DOWN) {
            if (p->n_items > 0u) {
                p->selected_index = (p->selected_index + 1u) % p->n_items;
                nd_pagedlist_draw(p);
            }
        } else if (key == ND_KEY_UP) {
            if (p->n_items > 0u) {
                p->selected_index = (p->selected_index + p->n_items - 1u) % p->n_items;
                nd_pagedlist_draw(p);
            }
        } else if (key == ND_KEY_ENTER) {
            /* Returns 0 even for an empty list, which draw() never renders a
             * row for. Latent in the Python and reproduced here. */
            return (int32_t)p->selected_index;
        } else if (key == ND_KEY_CLEAR) {
            return ND_WIDGET_BACK;
        }
    }
}
