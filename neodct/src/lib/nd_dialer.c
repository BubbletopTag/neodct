/* nd_dialer.c -- the two call screens, ported from System/ui/Dialer/.
 *
 * call_screen.py    the in-call screen: handset glyph, "Call 1", the number
 *                   under it, and an mm:ss timer once the line is up.
 * incoming_screen.py the 3310-style ring screen: the caller centred across
 *                   the top and a flashing "calling" near the bottom left.
 *
 * CORE PROCESS ONLY. nd_widgets.h says so and it is not a style rule: an app
 * process has ui->modem == NULL by nd_app.h's rules, so a call placed from
 * inside an app would draw a screen with nothing behind it. The core reclaims
 * the screen first (nd_proc signals the child, nd_main reaps it) and only then
 * calls in here.
 *
 * ============ THE DRAW HALVES DO NOT PRESENT ============
 *
 * nd_dialer_draw_call() and nd_dialer_draw_incoming() paint the canvas and
 * stop, because that is how shoot_docs.py's shoot_telephony() drives them --
 * it calls the draw helper and then flushes by hand. The golden frames
 * call-active and call-incoming are those two canvases with NO SOFTKEY BAR on
 * them at all; the status sprites overhang to y=147 and rows 148..174 are
 * black. Every pixel of that is a consequence of the loops not being involved,
 * so the split has to stay a split.
 *
 * nd_dialer_draw_incoming() takes blink_on explicitly for the same reason: the
 * blink is time-driven, and a captured frame has to be reproducible.
 *
 * ============ TWO QUIRKS THAT ARE PORTED, NOT FIXED ============
 *
 * 1. show_incoming's first frame has the blink OFF. blink_on starts True and
 *    the first thing the loop does is invert it, because last_blink is 0.0 and
 *    any real monotonic reading is more than BLINK_S past that. So the ring
 *    screen opens without the word "calling" on it and grows it half a second
 *    later. incoming_screen.py:100-115.
 *
 * 2. show_calling presents TWICE per 0.25 s frame -- once for the screen and
 *    once inside the softkey update, which defaults to present=True. It is
 *    visible on the panel as a two-stage repaint. call_screen.py:196-198.
 *
 * ============ SIX FITTERS, AND TWO OF THEM LIVE HERE ============
 *
 * nd_text.h's warning applies: these are checked against their own Python
 * originals, never against each other. fit_number() is call_screen's
 * _fit_text -- a binary search that appends U+2026 and falls back to the
 * ellipsis alone. fit_caller() is incoming_screen's _fit_caller_text -- a
 * font ladder that trims one character at a time, appends three ASCII dots,
 * and falls back to "?". They disagree about the ellipsis character, about
 * the give-up string and about whether a zero-length prefix is a candidate.
 * All three disagreements are in the Python.
 */

#include <string.h>

#include "nd_db.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_layout.h"
#include "nd_log.h"
#include "nd_modem.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

/* incoming_screen.py:16-19. KEY_ANSWER/KEY_DECLINE are the same 28/14 that
 * call_screen.py:215 hard-codes as its End keys. */
#define ND_DIALER_BLINK_S   0.5
#define ND_DIALER_REFRESH_S 0.1

/* call_screen.py:194 -- the in-call screen repaints four times a second so the
 * clock and the mm:ss timer advance. */
#define ND_DIALER_REDRAW_S 0.25

/* U+2026 HORIZONTAL ELLIPSIS, one character in Python and three bytes here.
 * The font has no glyph for it, so it draws nothing and still costs its
 * advance -- see nd_draw.h. That is the Python's behaviour too. */
#define ND_DIALER_ELLIPSIS "\xe2\x80\xa6"

/* ------------------------------------------------------------------ *
 * Small shared pieces
 * ------------------------------------------------------------------ */

static int32_t text_w(const nd_font *f, const char *s)
{
    int32_t w = 0;

    if (f == NULL || s == NULL)
        return 0;
    nd_text_size(f, s, &w, NULL);
    return w;
}

/* Python's // floors. (screen_w - w) goes negative for a caller string wider
 * than the panel, which _fit_caller_text can produce when even font_s cannot
 * be trimmed small enough, and that is the one case where floor and truncate
 * disagree. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

/* ------------------------------------------------------------------ *
 * call_screen._draw_handset_icon
 * ------------------------------------------------------------------ */

/* "Simple fallback icon (you can replace with a PNG later)" -- the Python's
 * own words. Three rectangles: an outline silhouette and the ear and mouth
 * blocks. Called with (8, 10), so the outline lands at (8,12)-(26,20). */
static void draw_handset_icon(nd_draw *d, int32_t x, int32_t y)
{
    (void)nd_draw_rect_outline(d, ND_RECT(x, y + 2, x + 18, y + 10), ND_WHITE, 1);
    (void)nd_draw_rect_fill(d, ND_RECT(x + 1, y + 3, x + 5, y + 5), ND_WHITE);
    (void)nd_draw_rect_fill(d, ND_RECT(x + 13, y + 7, x + 17, y + 9), ND_WHITE);
}

/* ------------------------------------------------------------------ *
 * call_screen._fit_text -- the binary-search fitter
 * ------------------------------------------------------------------ */

/* The Python bisects over prefix lengths and keeps the widest candidate that
 * fits. Every advance in this font is a non-negative whole number of pixels
 * (fontref.json fact 2) and the ellipsis is appended unconditionally, so
 * width(text[:n] + ell) is non-decreasing in n and "the widest that fits" is
 * also "the last that fits scanning up". Scanning up lets the UTF-8 be walked
 * once, forwards, which is the same trade nd_text.c makes.
 *
 * Writes the fitted string into out and returns the font to draw it with. */
static const nd_font *fit_number(const nd_ui *ui, char *out, size_t out_sz, const char *text,
                                 int32_t max_w, const nd_font *prefer)
{
    char cand[ND_TEXT_LINE_MAX];
    const nd_font *small;
    const char *p;
    size_t off = 0u;
    size_t best = 0u;
    bool found = false;

    if (out == NULL || out_sz == 0u)
        return prefer;
    out[0] = '\0';
    if (text == NULL)
        text = "";

    if (text_w(prefer, text) <= max_w) {
        (void)nd_strlcpy(out, text, out_sz);
        return prefer;
    }

    /* getattr(ui, "font_s", prefer_font) */
    small = (ui != NULL && ui->font_s != NULL) ? ui->font_s : prefer;

    if (text_w(small, text) <= max_w) {
        (void)nd_strlcpy(out, text, out_sz);
        return small;
    }

    /* `if not text: return "", small_font`. Only reachable when max_width is
     * negative, since an empty string measures zero. Ported anyway. */
    if (text[0] == '\0')
        return small;

    p = text;
    for (;;) {
        if (off + sizeof ND_DIALER_ELLIPSIS > sizeof cand)
            break;
        memcpy(cand, text, off);
        memcpy(cand + off, ND_DIALER_ELLIPSIS, sizeof ND_DIALER_ELLIPSIS);
        if (text_w(small, cand) > max_w)
            break;
        best = off;
        found = true;
        if (*p == '\0')
            break;
        (void)nd_utf8_next(&p);
        off = (size_t)(p - text);
    }

    if (!found) {
        /* `return best if best else ell` -- not even the bare ellipsis fits,
         * and the Python draws it anyway, over-wide. */
        (void)nd_strlcpy(out, ND_DIALER_ELLIPSIS, out_sz);
        return small;
    }

    memcpy(cand, text, best);
    memcpy(cand + best, ND_DIALER_ELLIPSIS, sizeof ND_DIALER_ELLIPSIS);
    (void)nd_strlcpy(out, cand, out_sz);
    return small;
}

/* ------------------------------------------------------------------ *
 * incoming_screen._fit_caller_text -- the font ladder
 * ------------------------------------------------------------------ */

/* "Largest font that fits, truncating only as a last resort." The ladder is
 * font_n then font_s -- NOT nd_font_ladder()'s 20/18/14, which includes
 * font_md. incoming_screen.py:46 names the two attributes literally. */
static const nd_font *fit_caller(const nd_ui *ui, char *out, size_t out_sz, const char *text,
                                 int32_t max_w)
{
    char cand[ND_TEXT_LINE_MAX];
    const nd_font *font;
    const char *p;
    size_t off = 0u;
    size_t best = 0u;
    bool found = false;

    if (out == NULL || out_sz == 0u || ui == NULL)
        return NULL;
    out[0] = '\0';
    if (text == NULL)
        text = "";

    if (ui->font_n != NULL && text_w(ui->font_n, text) <= max_w) {
        (void)nd_strlcpy(out, text, out_sz);
        return ui->font_n;
    }
    if (ui->font_s != NULL && text_w(ui->font_s, text) <= max_w) {
        (void)nd_strlcpy(out, text, out_sz);
        return ui->font_s;
    }

    /* getattr(ui, "font_s", None) or getattr(ui, "font_n", None) */
    font = (ui->font_s != NULL) ? ui->font_s : ui->font_n;

    /* `while trimmed and get_text_size(trimmed + "...") > max_width` -- the
     * loop stops at the empty string WITHOUT testing it, so a zero-length
     * prefix is never a candidate here. fit_number's bisect does test one.
     * Same monotonicity argument as there, so the scan runs forwards. */
    p = text;
    for (;;) {
        if (*p == '\0')
            break;
        (void)nd_utf8_next(&p);
        off = (size_t)(p - text);
        if (off + 4u > sizeof cand)
            break;
        memcpy(cand, text, off);
        memcpy(cand + off, "...", 4u);
        if (text_w(font, cand) > max_w)
            break;
        best = off;
        found = true;
    }

    if (!found) {
        /* `return (trimmed + "..." if trimmed else "?"), font` */
        (void)nd_strlcpy(out, "?", out_sz);
        return font;
    }

    memcpy(cand, text, best);
    memcpy(cand + best, "...", 4u);
    (void)nd_strlcpy(out, cand, out_sz);
    return font;
}

/* ------------------------------------------------------------------ *
 * The status chrome both screens borrow from the home layout
 * ------------------------------------------------------------------ */

/* call_screen.py:161-168 and incoming_screen.py:83-86 reach into the parsed
 * ui_home.json and re-render its elements so the clock and the sprites land
 * at EXACTLY their home placement rather than at hand-written coordinates.
 * with_clock is the difference between the two screens: the in-call screen
 * draws the first "12:00" text element and stops looking; the ring screen
 * draws no clock at all. */
static void render_status_chrome(nd_ui *ui, bool with_clock)
{
    const nd_home_layout *layout = nd_ui_home_layout(ui);
    size_t i;

    if (layout == NULL)
        return;

    if (with_clock) {
        for (i = 0u; i < layout->n_elements; i++) {
            const nd_element *el = &layout->elements[i];

            if (el->type == ND_EL_TEXT && strcmp(el->text, "12:00") == 0) {
                nd_home_render_element(ui, el);
                break; /* `break` in the Python: the first one wins */
            }
        }
    }

    for (i = 0u; i < layout->n_elements; i++) {
        const nd_element *el = &layout->elements[i];

        if (el->type == ND_EL_ICON_SET &&
            (strcmp(el->prefix, "bat") == 0 || strcmp(el->prefix, "sig") == 0))
            nd_home_render_element(ui, el);
    }
}

/* ------------------------------------------------------------------ *
 * draw_call_screen
 * ------------------------------------------------------------------ */

void nd_dialer_draw_call(nd_ui *ui, const char *number, const char *name)
{
    char fitted[ND_TEXT_LINE_MAX];
    nd_draw *d;
    const nd_font *label_font;
    const nd_font *num_font;
    const char *status = "CONNECTED";
    const char *label;
    int32_t secs = -1;
    int32_t screen_w;
    int32_t content_bottom;
    int32_t label_x;
    int32_t label_y;
    int32_t num_y;

    /* draw_call_screen never reads `name`; the Python's own comment says the
     * contact name is "keeping it off by default to match your request". The
     * parameter is kept because show_calling passes it through. */
    ND_UNUSED(name);

    if (ui == NULL || ui->draw == NULL)
        return;
    d = ui->draw;
    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    /* The FULL screen, softkey strip included -- unlike most widgets, which
     * clear rows 0..content_bottom only. call_screen.py:118. */
    nd_ui_paint_chrome_full(ui);

    draw_handset_icon(d, 8, 10);

    /* The top-right clock is COMMENTED OUT at call_screen.py:125-127 and the
     * home layout's own clock element is rendered at the bottom of the
     * function instead. Both are ported: nothing here, one there. */

    /* Live progress from AT+CLCC: Calling... -> Ringing... -> Call 1.
     * ("CONNECTED", None) is the default when there is no modem at all. */
    if (ui->modem != NULL)
        nd_modem_call_status(ui->modem, &status, &secs);

    if (strcmp(status, "CALLING") == 0)
        label = "Calling...";
    else if (strcmp(status, "RINGING") == 0)
        label = "Ringing...";
    else
        label = "Call 1";

    label_font = (ui->font_n != NULL) ? ui->font_n : ui->font_s;

    label_x = nd_max32(34, nd_trunc32((double)screen_w * 0.23));
    label_y = nd_max32(50, nd_trunc32((double)content_bottom * 0.20));
    (void)nd_draw_text(d, label_x, label_y, label, label_font, ND_WHITE);

    /* The number goes directly under the label, fitted into what is left of
     * the width with a 10 px right margin. */
    num_font = fit_number(ui, fitted, sizeof fitted, number != NULL ? number : "",
                          screen_w - label_x - 10, label_font);
    num_y = label_y + 26; /* "Nokia-ish spacing under the label" */
    (void)nd_draw_text(d, label_x, num_y, fitted, num_font, ND_WHITE);

    /* Connected: a running mm:ss under the number, in grey. secs is -1 for
     * the Python's None, which is every state but CONNECTED. */
    if (secs >= 0) {
        char timer_text[16];
        const nd_font *timer_font = (ui->font_s != NULL) ? ui->font_s : num_font;

        if (nd_snprintf(timer_text, sizeof timer_text, "%02d:%02d", secs / 60, secs % 60) == ND_OK)
            (void)nd_draw_text(d, label_x, num_y + 24, timer_text, timer_font, ND_GRAY);
    }

    render_status_chrome(ui, /*with_clock=*/true);
}

/* ------------------------------------------------------------------ *
 * draw_incoming_screen
 * ------------------------------------------------------------------ */

void nd_dialer_draw_incoming(nd_ui *ui, const char *caller_text, bool blink_on)
{
    char fitted[ND_TEXT_LINE_MAX];
    nd_draw *d;
    const nd_font *font;
    int32_t screen_w;
    int32_t screen_h;
    int32_t content_bottom;
    int32_t w;

    if (ui == NULL || ui->draw == NULL)
        return;
    d = ui->draw;
    screen_w = nd_ui_width(ui);
    screen_h = nd_ui_height(ui);
    content_bottom = nd_ui_content_bottom(ui);

    nd_ui_paint_chrome_full(ui);

    /* Caller: name or number, centred across the top by INK width, so the
     * centring shifts with which letters are in it. */
    font = fit_caller(ui, fitted, sizeof fitted, caller_text, screen_w - 16);
    w = text_w(font, fitted);
    (void)nd_draw_text(d, floordiv2(screen_w - w),
                       nd_max32(18, nd_trunc32((double)content_bottom * 0.18)), fitted, font,
                       ND_WHITE);

    /* Flashing "calling" near the bottom left, like the 3310 -- starting just
     * right of the signal column. The 36 is the asset's authored width and it
     * is scaled by H/240 exactly the way render_element scales status icons,
     * so this tracks the sprite if the panel ever changes. 7 + 26 + 6 = 39
     * on this one. */
    if (blink_on) {
        int32_t calling_x = 7 + nd_trunc32(36.0 * ((double)screen_h / 240.0)) + 6;
        const nd_font *label_font = (ui->font_n != NULL) ? ui->font_n : font;

        (void)nd_draw_text(d, calling_x, content_bottom - 26, "calling", label_font, ND_WHITE);
    }

    /* Status icons stay put, same as the in-call screen. No clock here. */
    render_status_chrome(ui, /*with_clock=*/false);
}

/* ------------------------------------------------------------------ *
 * show_calling
 * ------------------------------------------------------------------ */

void nd_dialer_show_calling(nd_ui *ui, const char *number, const char *name)
{
    nd_softkey bar;
    double last_draw = 0.0;

    if (ui == NULL)
        return;

    /* `use_ui_reader = hasattr(ui, "read_keypress")` is always true in C --
     * nd_ui_read_keypress() is a function, not an optional attribute -- so
     * call_screen's legacy branch (open /dev/input/event0 itself, drain it
     * with _flush_input) is unreachable and is not ported. The core owns the
     * keypad by nd_keypad.h's contract; nothing else may open it. */
    nd_softkey_init(&bar, ui, false);

    for (;;) {
        double now = nd_time_now();
        int32_t key;

        if (now - last_draw >= ND_DIALER_REDRAW_S) {
            nd_dialer_draw_call(ui, number, name);
            (void)nd_ui_present(ui);
            last_draw = now;
            /* present defaults to True: this is the second present of the
             * frame and the two-stage repaint is deliberate. */
            nd_softkey_update(&bar, "End", true);
        }

        key = nd_ui_read_keypress(ui, ND_DIALER_REFRESH_S);

        /* Remote hangup / call failure: the modem thread has already seen the
         * NO CARRIER or VOICE CALL: END urc, so IDLE means it is over. */
        if (ui->modem != NULL && nd_modem_state(ui->modem) == ND_CALL_IDLE)
            return;

        /* `if key is None: continue`. ND_KEY_NONE is the timeout;
         * ND_KEY_INCOMING_CALL cannot arrive because ui->handling_call gags
         * the ring tick, and is treated as "keep waiting" either way. */
        if (key < 0)
            continue;

        if (key == ND_KEY_CLEAR || key == ND_KEY_ENTER) {
            if (ui->modem != NULL)
                (void)nd_modem_hangup(ui->modem);
            return;
        }
    }
}

/* ------------------------------------------------------------------ *
 * show_incoming
 * ------------------------------------------------------------------ */

/* `name or _lookup_contact_name(num) or num or "Unknown"`. An empty string is
 * falsy in Python, so "" falls through to the next term exactly as None does.
 *
 * The phonebook lookup is nd_contacts_lookup_name(), which is the same
 * digits-only, last-ten-digits comparison incoming_screen._lookup_contact_name
 * performs; nd_db.h's comment cites this function as its reason for existing.
 * A missing or unreadable database is "no match", not a crash -- the Python
 * swallows every exception around the import and the query. */
static void caller_label(const char *name, const char *number, char *out, size_t out_sz)
{
    char found[ND_CONTACT_NAME_MAX];

    if (name != NULL && name[0] != '\0') {
        (void)nd_strlcpy(out, name, out_sz);
        return;
    }
    if (number != NULL && number[0] != '\0' &&
        nd_contacts_lookup_name(number, found, sizeof found) && found[0] != '\0') {
        (void)nd_strlcpy(out, found, out_sz);
        return;
    }
    if (number != NULL && number[0] != '\0') {
        (void)nd_strlcpy(out, number, out_sz);
        return;
    }
    (void)nd_strlcpy(out, "Unknown", out_sz);
}

nd_incoming_result nd_dialer_show_incoming(nd_ui *ui, const char *number, const char *name)
{
    nd_softkey bar;
    /* The Python rebinds `number` to whatever a late +CLIP brings, so this is
     * a copy rather than the caller's pointer. ND_MODEM_NUMBER_MAX is the
     * modem's own caller-ID field width, so nothing the modem can report is
     * truncated by it. */
    char num[ND_MODEM_NUMBER_MAX];
    char caller_text[ND_TEXT_LINE_MAX];
    /* Starts True and the first pass through the loop inverts it, so the ring
     * screen opens with "calling" OFF. Quirk 1 in the header comment. */
    bool blink_on = true;
    double last_blink = 0.0;

    if (ui == NULL)
        return ND_CALL_GONE;

    nd_softkey_init(&bar, ui, false);
    (void)nd_strlcpy(num, number != NULL ? number : "", sizeof num);
    caller_label(name, num, caller_text, sizeof caller_text);

    for (;;) {
        double now = nd_time_monotonic();
        const char *cid = (ui->modem != NULL) ? nd_modem_caller_id(ui->modem) : NULL;
        int32_t key;

        /* A late +CLIP -- caller ID lands just after the first RING -- fills
         * in the name without waiting for the next call. */
        if (cid != NULL && cid[0] != '\0' && strcmp(cid, num) != 0) {
            (void)nd_strlcpy(num, cid, sizeof num);
            caller_label(name, num, caller_text, sizeof caller_text);
            last_blink = 0.0; /* redraw now */
        }

        if (now - last_blink >= ND_DIALER_BLINK_S) {
            blink_on = !blink_on;
            last_blink = now;
            nd_dialer_draw_incoming(ui, caller_text, blink_on);
            /* present=False here, and the fb update below is the one commit.
             * Unlike show_calling this frame is pushed exactly once. */
            nd_softkey_update(&bar, "Answer", false);
            (void)nd_ui_present(ui);
        }

        /* read_keypress pumps the modem, so URCs land while we ring. */
        key = nd_ui_read_keypress(ui, ND_DIALER_REFRESH_S);

        /* `modem.state not in ("RINGING", "INCOMING")`. nd_call_state has no
         * INCOMING member, so RINGING is the whole of the still-ringing set.
         * Anything else that is not IDLE means somebody -- the far end, or
         * another thread -- moved us to a live call, and the Python reports
         * that as "answered" even from CALLING. Ported as it is. */
        if (ui->modem != NULL) {
            nd_call_state st = nd_modem_state(ui->modem);

            if (st != ND_CALL_RINGING)
                return (st == ND_CALL_IDLE) ? ND_CALL_GONE : ND_CALL_ANSWERED;
        }

        if (key == ND_KEY_ENTER)
            return ND_CALL_ANSWERED;
        if (key == ND_KEY_CLEAR)
            return ND_CALL_DECLINED;
    }
}
