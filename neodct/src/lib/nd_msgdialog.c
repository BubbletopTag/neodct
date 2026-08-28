/* nd_msgdialog.c -- MessageDialog, the full-screen warning.
 *
 * Ported from System/ui/framework.py:990. Fifteen call sites -- ErrorScreen,
 * CrashHandler, Power, Update, Downgrade, Tones, Clock, Messages,
 * MusicPlayer, RemoteShell, Modem, FuelGauge, both KeypadMappers and
 * TestsApp -- plus, for this session, all 25 stub apps: SESSION-SCOPE.md makes
 * this widget the entire visible behaviour of every unimplemented app, so
 * golden/widget-messagedialog.png is the frame the owner sees 25 times.
 *
 * ============ IT CLEARS THE WHOLE SCREEN, NOT ROWS 0..145 ============
 *
 * Two widgets in the framework clear 0..175 including the softkey strip:
 * this one and PagedList. Everything else clears 0..145 so that a caller's
 * earlier nd_softkey_update(..., present=false) survives into the frame. Here
 * the dialog paints its own bar afterwards, so the full clear is correct and
 * a partial one would leave the previous screen's label showing through.
 *
 * ============ TWO LOOKS, CHOSEN BY THE 20 px WRAP ============
 *
 * The message is wrapped at 20 px first. Two lines or fewer and it stays at
 * 20 px, centred -- the Nokia alert look that "LOW BATTERY!" wants. Three or
 * more and the whole thing is re-wrapped at 14 px and left-aligned at the
 * margin, because a paragraph centred line by line is unreadable.
 *
 * The C only needs to know WHETHER the 20 px wrap exceeded two lines, so the
 * first wrap runs into a three-line buffer and asks nd_lines for `truncated`.
 * That is cheaper than the Python's full wrap and cannot differ: both tests
 * are "more than two".
 *
 * ============ THE INVISIBLE ELLIPSIS ============
 *
 * When the clipped text does not fit, the last line gets " …" appended --
 * U+2026, which this font has no glyph for. It draws NOTHING and still costs
 * 8 px of advance at 20 px. Writing "..." instead would be three visible dots
 * and a different pixel count, so the codepoint is reproduced verbatim.
 * spec-ui-framework.md risk 8 says the same thing.
 *
 * ============ THE FLUSH IS 0.0, NOT 0.01 ============
 *
 * AppSelector and PagedList poll their input with a 0.01 s timeout, which
 * catches a record that was already on its way. MessageDialog polls with 0.0
 * and stops at the first idle poll -- a plain non-blocking drain. nd_input.h
 * is explicit that the two numbers stay apart, so this uses nd_input_drain()
 * and PagedList does not.
 */

#include <string.h>

#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_paths.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

/* The paragraph look draws at most (145 - 8 - 8) / 18 = 7 lines and the alert
 * look at most (145 - 8 - 8) / 24 = 5, so 24 is four times what any dialog on
 * this panel can show. Sized as a ceiling rather than a guess because
 * max_lines is clamped to it below: a font whose "Ag" measured absurdly short
 * would otherwise ask for more lines than the buffer holds and lose text
 * silently instead of ending in the ellipsis. 24 * 256 = 6,144 bytes. */
#define DIALOG_MAX_LINES 24

/* Python's // floors; C's / truncates toward zero. Both centring expressions
 * below can go negative -- a message wider than the screen, or a body taller
 * than the space left for it -- and that is exactly where they disagree. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

void nd_msgdialog_init(nd_msgdialog *d, nd_ui *ui, const char *message)
{
    if (d == NULL)
        return;

    memset(d, 0, sizeof *d);
    d->ui = ui;
    /* `self.message = message or ""`. */
    d->message = (message != NULL) ? message : "";
    d->title = NULL;
    d->icon_path = NULL; /* resolved to the warning triangle at draw time */
    d->button_text = "OK";
    d->accept_keys[0] = ND_KEY_ENTER;
    d->n_accept = 1u;
    d->cancel_keys[0] = ND_KEY_CLEAR;
    d->n_cancel = 1u;
    d->margin = 8;
}

void nd_msgdialog_set_title(nd_msgdialog *d, const char *title)
{
    if (d != NULL)
        d->title = title;
}

void nd_msgdialog_set_icon(nd_msgdialog *d, const char *icon_path)
{
    if (d != NULL)
        d->icon_path = icon_path;
}

void nd_msgdialog_set_button(nd_msgdialog *d, const char *button_text)
{
    if (d != NULL)
        d->button_text = button_text;
}

void nd_msgdialog_set_keys(nd_msgdialog *d, const int32_t *accept, size_t n_accept,
                           const int32_t *cancel, size_t n_cancel)
{
    size_t i;

    if (d == NULL)
        return;

    /* `tuple(accept_keys or ())` -- a caller passing an empty set gets an
     * un-cancellable notice, which is what the low-battery shutdown wants. */
    if (accept == NULL)
        n_accept = 0u;
    if (cancel == NULL)
        n_cancel = 0u;
    if (n_accept > ND_DIALOG_KEYS_MAX)
        n_accept = ND_DIALOG_KEYS_MAX;
    if (n_cancel > ND_DIALOG_KEYS_MAX)
        n_cancel = ND_DIALOG_KEYS_MAX;

    for (i = 0u; i < n_accept; i++)
        d->accept_keys[i] = accept[i];
    for (i = 0u; i < n_cancel; i++)
        d->cancel_keys[i] = cancel[i];
    d->n_accept = n_accept;
    d->n_cancel = n_cancel;
}

/* framework.py:1016. Drain pending key records so the key that opened this
 * screen does not immediately dismiss it. */
static void flush_input(nd_msgdialog *d)
{
    if (d->ui != NULL && d->ui->input != NULL)
        nd_input_drain(d->ui->input);
}

/* getattr(ui, "font_md") or font_n or font_s -- the 18 px face, with the same
 * two fallbacks the Python spells out. */
static const nd_font *title_font_of(const nd_ui *ui)
{
    if (ui->font_md != NULL)
        return ui->font_md;
    if (ui->font_n != NULL)
        return ui->font_n;
    return ui->font_s;
}

/* font_s or font_n -- the 14 px paragraph face. */
static const nd_font *body_font_of(const nd_ui *ui)
{
    return (ui->font_s != NULL) ? ui->font_s : ui->font_n;
}

/* Append " …" unless the line already ends in one. U+2026 is E2 80 A6. */
static void append_ellipsis(char *line, size_t cap)
{
    static const char DOTS[] = "\xE2\x80\xA6";
    size_t len = strlen(line);

    if (len >= 3u && memcmp(line + len - 3u, DOTS, 3u) == 0)
        return;
    (void)nd_strlcat(line, " ", cap);
    (void)nd_strlcat(line, DOTS, cap);
}

static void dialog_draw(nd_msgdialog *d)
{
    nd_ui *ui;
    nd_draw *dr;
    int32_t screen_w;
    int32_t content_bottom;
    const nd_image *icon = NULL;
    const nd_font *font_title;
    const nd_font *font_body;
    const nd_font *alert_font;
    const char *icon_path;
    int32_t y;
    int32_t max_w;
    int32_t line_h;
    int32_t max_lines;
    int32_t ag_h = 0;
    bool centered;
    nd_softkey bar;
    size_t i;

    char alert_store[3][ND_TEXT_LINE_MAX];
    nd_lines alert;
    char body_store[DIALOG_MAX_LINES][ND_TEXT_LINE_MAX];
    nd_lines lines;

    if (d == NULL || d->ui == NULL || d->ui->draw == NULL || d->ui->canvas == NULL)
        return;

    ui = d->ui;
    dr = ui->draw;
    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    /* 1. Full clear -- rows 0..175, softkey strip included. */
    nd_ui_paint_chrome_full(ui);

    /* 2. Icon. `icon_path or DEFAULT_WARNING_ICON`: NULL and "" both give the
     *    triangle, and there is no way to ask for no icon short of a path
     *    that fails to load. No max_size, so the full-size art is cached. */
    icon_path =
        (d->icon_path != NULL && d->icon_path[0] != '\0') ? d->icon_path : ND_PATH_WARNING_ICON;
    icon = nd_ui_get_image(ui, icon_path);
    if (icon != NULL) {
        if (icon->fmt == ND_PIXFMT_RGBA8888)
            (void)nd_image_blit_alpha(ui->canvas, icon, d->margin, d->margin);
        else
            (void)nd_image_blit(ui->canvas, icon, d->margin, d->margin);
    }

    /* 3. Title. */
    y = d->margin;
    font_title = title_font_of(ui);
    if (d->title != NULL && d->title[0] != '\0' && font_title != NULL) {
        int32_t th = 0;
        int32_t title_x = d->margin + ((icon != NULL) ? icon->w + 6 : 0);

        (void)nd_draw_text(dr, title_x, d->margin, d->title, font_title, ND_WHITE);
        nd_text_size(font_title, d->title, NULL, &th);
        y = nd_max32(y, d->margin + th + 6);
    }
    if (icon != NULL) {
        /* The body must clear the icon even when the title is shorter than it,
         * or the first line lands on the triangle. */
        y = nd_max32(y, d->margin + icon->h + 6);
    }

    /* 4. Which look. The 20 px wrap only has to answer "more than two lines?",
     *    so it runs into a three-line buffer: n > 2, or the buffer overflowed,
     *    both mean the paragraph form. */
    max_w = screen_w - (d->margin * 2);
    alert_font = (ui->font_n != NULL) ? ui->font_n : body_font_of(ui);
    nd_lines_init(&alert, alert_store, ND_ARRAY_LEN(alert_store));
    nd_text_wrap_break_pop(&alert, d->message, alert_font, max_w);

    nd_lines_init(&lines, body_store, ND_ARRAY_LEN(body_store));
    if (alert.n <= 2u && !alert.truncated) {
        font_body = alert_font;
        centered = true;
        for (i = 0u; i < alert.n; i++)
            (void)nd_lines_push(&lines, nd_lines_at(&alert, i));
    } else {
        font_body = body_font_of(ui);
        centered = false;
        nd_text_wrap_break_pop(&lines, d->message, font_body, max_w);
    }
    if (font_body == NULL)
        return;

    /* 5. Line height from the INK of "Ag", so the two looks are 24 and 18. */
    nd_text_size(font_body, "Ag", NULL, &ag_h);
    line_h = ag_h + 3;

    /* 5b. Clip, and mark the clip with the invisible ellipsis. int() truncates
     *     toward zero, which is why this is a double divide and not / . */
    max_lines = nd_max32(1, nd_trunc32((double)(content_bottom - y - d->margin) / (double)line_h));
    if (max_lines > (int32_t)ND_ARRAY_LEN(body_store))
        max_lines = (int32_t)ND_ARRAY_LEN(body_store);
    if (lines.n > (size_t)max_lines || lines.truncated) {
        if (lines.n > (size_t)max_lines)
            lines.n = (size_t)max_lines;
        if (lines.n > 0u)
            append_ellipsis(body_store[lines.n - 1u], ND_TEXT_LINE_MAX);
    }

    /* 6. Vertically centre the body in the space above the softkey. */
    y += nd_max32(0, floordiv2(content_bottom - d->margin - y - (int32_t)lines.n * line_h));

    /* 7. The lines themselves. */
    for (i = 0u; i < lines.n; i++) {
        const char *line = nd_lines_at(&lines, i);
        int32_t x = d->margin;

        if (centered) {
            int32_t lw = 0;

            nd_text_size(font_body, line, &lw, NULL);
            x = nd_max32(d->margin, floordiv2(screen_w - lw));
        }
        (void)nd_draw_text(dr, x, y, line, font_body, ND_WHITE);
        y += line_h;
    }

    /* 8. A FRESH bar, so transparency is re-decided here -- and it is always
     *    opaque, because only the core's own bar is transparent. Painted, not
     *    presented; step 9 is the single present. */
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, d->button_text, false);

    /* 9. Present once. */
    (void)nd_ui_present(ui);
}

void nd_msgdialog_render(nd_msgdialog *d)
{
    if (d == NULL)
        return;
    flush_input(d);
    dialog_draw(d);
}

static bool key_in(const int32_t *keys, size_t n, int32_t key)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (keys[i] == key)
            return true;
    }
    return false;
}

int32_t nd_msgdialog_show(nd_msgdialog *d)
{
    if (d == NULL)
        return ND_KEY_NONE;

    flush_input(d);
    dialog_draw(d);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(d->ui);

        /* Any other key is ignored with NO redraw. An un-cancellable notice
         * (n_accept == n_cancel == 0) therefore never returns, which is what
         * the low-battery shutdown wants -- it is waiting for the power to
         * go, not for a key. */
        if (key_in(d->accept_keys, d->n_accept, key) || key_in(d->cancel_keys, d->n_cancel, key))
            return key;
    }
}
