/* nd_widgets.h -- the fourteen screen parts the whole phone is built from.
 *
 * Nothing here knows what a phonebook or a text message is. It knows how to
 * draw a menu, a text box, a warning and a progress bar, and how to wait for
 * you to press a key.
 *
 * ============ HOW A WIDGET IS USED ============
 *
 * Every widget is a caller-owned struct on the caller's stack. init() fills it
 * in and allocates nothing; show() paints, loops on keys, and returns an
 * answer. Strings passed to init are BORROWED -- they must outlive the widget.
 *
 *     nd_vlist list;
 *     nd_vlist_init(&list, ui, "Messages", items, n_items, 2);
 *     int32_t choice = nd_vlist_show(&list);   // >= 0, or -1 for Back
 *
 * The one exception is nd_detailpage, which lays a page out into a bounded
 * block array; see its section.
 *
 * ============ FOUR RULES THAT DECIDE WHETHER IT LOOKS RIGHT ============
 *
 * 1. PARTIAL CLEARS ARE LOAD-BEARING. Most widgets clear rows 0..145 only, so
 *    a caller's earlier nd_softkey_update(..., present=false) survives into
 *    the frame. MessageDialog and PagedList clear the full 0..175 instead.
 *    Getting either wrong loses or double-draws the softkey.
 *
 * 2. TEXT IS MEASURED BY ITS INK. nd_text_size() returns the ink box of that
 *    specific string, so centring visibly shifts depending on which letters
 *    are in it. "_" is 3 px tall at 20 px; "Ag" is 21. That is not a bug.
 *
 * 3. SCROLLBAR NOTCHES TRUNCATE. notch_y = track_top + selected * step is a
 *    float and Pillow truncates it. Compute in double, cast with nd_trunc32().
 *
 * 4. SIX WIDGETS BUILD A FRESH SOFTKEY BAR INSIDE draw(), and three of those
 *    then present a second time. The double present is visible on the panel as
 *    a two-stage repaint. Keep it.
 */

#ifndef ND_WIDGETS_H_INCLUDED
#define ND_WIDGETS_H_INCLUDED

#include "nd_t9.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Every list-like widget returns a zero-based index, or this for Back. */
#define ND_WIDGET_BACK (-1)

typedef enum {
    ND_WIDGET_RESULT_NONE = 0, /* the key was ignored; do not redraw   */
    ND_WIDGET_RESULT_TYPED,
    ND_WIDGET_RESULT_BACKSPACE,
    ND_WIDGET_RESULT_EMPTY_BACKSPACE,
    ND_WIDGET_RESULT_MODE,
    ND_WIDGET_RESULT_CONFIRM,
    ND_WIDGET_RESULT_CANCEL
} nd_widget_result;

/* ================================================================== *
 * 1. SoftKeyBar -- the 30 px strip along the bottom
 * ================================================================== */

typedef struct nd_softkey {
    nd_ui *ui;
    int32_t height;  /* 30  */
    int32_t y_start; /* 145 */
    bool transparent;
    char current_text[64];
    bool has_text;
} nd_softkey;

/* transparent is EXPLICIT here. Exactly one bar in the system is transparent:
 * the core's own, built during nd_ui_init step 9. Everything else is opaque.
 * See the nd_ui.h header comment for why. */
void nd_softkey_init(nd_softkey *bar, nd_ui *ui, bool transparent);

/* Repaint the strip and optionally present.
 *
 * Transparent + a wallpaper: the wallpaper's own rows 145..175 are pasted
 * back. Otherwise the strip is filled black.
 *
 * text NULL or "" both clear the strip and draw NOTHING -- ProgressScreen and
 * PagedList's empty state depend on that, so do not treat "" as an error. */
void nd_softkey_update(nd_softkey *bar, const char *text, bool present);

/* ================================================================== *
 * 2. HeaderWidget -- the "1-4" breadcrumb in the top-right corner
 * ================================================================== */

/* root_id is a STRING, because callers pass both integers ("1") and compound
 * ids ("5-5", "1-6"). Formatting is Python's "%s", so an integer id has no
 * padding. */
typedef struct {
    nd_ui *ui;
    char root_id[16];
} nd_header;

void nd_header_init(nd_header *h, nd_ui *ui, const char *root_id);
void nd_header_init_int(nd_header *h, nd_ui *ui, int32_t root_id);

/* "<root>-<sub>" when sub >= 0, "<root>" otherwise. */
void nd_header_text_for(const nd_header *h, int32_t sub_index, char *out, size_t out_sz);

/* 5 + ink width. The 5 is the right margin, and callers subtract this whole
 * number to know where a title must stop. */
int32_t nd_header_width(const nd_header *h, int32_t sub_index);

void nd_header_draw(const nd_header *h, int32_t sub_index);

/* ================================================================== *
 * 3. AppSelector -- the main menu, one big icon at a time
 * ================================================================== */

/* NOTE THE ARGUMENT ORDER in the Python: AppSelector(title, items, ui). It is
 * the only widget that does not take ui first. In C ui comes first like
 * everything else, because with all-pointer arguments a transposition would
 * compile. Saying so loudly here is the mitigation.
 *
 * `title` is accepted and NEVER DRAWN -- the header shows the current app's
 * name instead. Kept for source-level correspondence. */
typedef struct {
    nd_ui *ui;
    const char *title;
    const nd_app_entry *items;
    size_t n_items;
    const nd_image *background; /* NULL for a black background */
    size_t selected_index;
} nd_appsel;

void nd_appsel_init(nd_appsel *s, nd_ui *ui, const char *title, const nd_app_entry *items,
                    size_t n_items, const nd_image *background);
void nd_appsel_draw(nd_appsel *s);

/* Flushes pending input (select with a 0.01 s timeout), draws, then loops.
 * Down/Up wrap with modulo; Enter returns the index; Clear returns
 * ND_WIDGET_BACK. With an EMPTY list only Clear and Enter respond, and both
 * return ND_WIDGET_BACK -- that guard is what stops the modulo by zero. */
int32_t nd_appsel_show(nd_appsel *s);

/* ================================================================== *
 * 4. VerticalList -- the ordinary three-row menu
 * ================================================================== */

typedef struct {
    nd_ui *ui;
    nd_header header;
    const char *title;
    const char *const *items;
    size_t n_items;
    size_t selected_index;
    size_t window_start;
    size_t max_lines; /* recomputed in draw; 3 on this panel */
} nd_vlist;

void nd_vlist_init(nd_vlist *l, nd_ui *ui, const char *title, const char *const *items,
                   size_t n_items, int32_t app_id);
void nd_vlist_draw(nd_vlist *l);
int32_t nd_vlist_show(nd_vlist *l);

/* One key, without drawing -- so a caller can compose its own loop the way
 * Messages does. Returns the chosen index, ND_WIDGET_BACK, or
 * ND_WIDGET_RESULT_NONE-equivalent (-2) for "keep going". */
#define ND_VLIST_CONTINUE (-2)
int32_t nd_vlist_handle_key(nd_vlist *l, int32_t key);

/* ================================================================== *
 * 5. LevelSelector -- a VerticalList of "Level 1".."Level N"
 * ================================================================== */

#define ND_LEVELSEL_MAX 9

typedef struct {
    nd_vlist list;
    char labels[ND_LEVELSEL_MAX][16];
    const char *label_ptrs[ND_LEVELSEL_MAX];
    int32_t count;
} nd_levelsel;

void nd_levelsel_init(nd_levelsel *s, nd_ui *ui, int32_t current, int32_t count, const char *title,
                      int32_t app_id);

/* Draws an "OK" softkey WITHOUT presenting, then runs the list -- whose draw()
 * clears only rows 0..145, so the "OK" survives and is presented with the
 * list. Returns the level (1-based), or -1 for Back. */
int32_t nd_levelsel_show(nd_levelsel *s);

/* ================================================================== *
 * 6. PagedList -- one item per screen, in big type
 * ================================================================== */

typedef struct {
    nd_ui *ui;
    nd_header header;
    nd_softkey softkey;
    bool show_select_hint;
    const char *title;
    const char *const *items;
    size_t n_items;
    size_t selected_index;
    int32_t content_top;    /* 38  */
    int32_t content_bottom; /* 135 */
    int32_t bar_x;          /* 235 */
} nd_pagedlist;

void nd_pagedlist_init(nd_pagedlist *p, nd_ui *ui, const char *title, const char *const *items,
                       size_t n_items, const char *root_id, bool show_select_hint);
void nd_pagedlist_draw(nd_pagedlist *p);
int32_t nd_pagedlist_show(nd_pagedlist *p);

/* The FOURTH wrapper, and different from the three in nd_text.h: it splits on
 * ANY whitespace with no empty tokens, so newlines and runs of spaces collapse
 * entirely. It also appends "..." to a hard-trimmed word only when that word
 * is NOT the last one. Both quirks are visible on the Call Log. */
void nd_pagedlist_wrap(nd_lines *out, const char *text, const nd_font *f, int32_t max_w,
                       size_t max_lines);

/* ================================================================== *
 * 7. PredictiveText -- the shared brain behind the two text boxes
 * ================================================================== */

/* Not a screen. Embedded in both text widgets. Tracks how many characters at
 * the insertion point are provisional (the underlined guess) and which
 * candidate is showing. */
typedef struct {
    size_t pending_len;
    size_t candidate_idx;
    size_t n_candidates;
    char candidates[ND_T9_MAX_SUGGESTIONS][ND_T9_WORD_MAX];
} nd_predictive;

/* Clears the pending length, the candidates AND the engine's accumulated
 * digits. BOTH HALVES OR NEITHER -- leaving digits in the engine makes the
 * next key continue a word the field has already thrown away. */
void nd_predictive_reset(nd_predictive *p, nd_t9_engine *t9);

/* ================================================================== *
 * 8. TextInput -- a one-line field
 * ================================================================== */

/* Neither Python text widget limits its length. The C must, so:
 * 256 bytes for a name or a host name, 1024 for the SMS composer (the spec
 * implies at least 480). Both recorded in OPEN-QUESTIONS.md. */
#define ND_TEXTINPUT_CAP 256
#define ND_TEXTLONG_CAP  1024

typedef struct {
    nd_ui *ui;
    const char *title; /* drawn WITHOUT fit_text: a long title runs off the
                           * right edge. Port the bug. */
    const char *prompt;
    char *text; /* caller-owned buffer, NUL-terminated */
    size_t cap; /* its capacity in bytes               */
    nd_t9_engine t9;
    nd_predictive predict;
} nd_textinput;

/* filter is passed straight to the T9 engine and also gates the QWERTY path. */
nd_err nd_textinput_init(nd_textinput *t, nd_ui *ui, const char *title, const char *prompt,
                         char *text_buf, size_t cap, const char *initial, nd_t9_filter filter);

/* blink_state draws the trailing "_" cursor. Because the cursor changes the
 * measured INK HEIGHT of the line, the text visibly jumps as you type:
 * empty + cursor is 3 px tall, "Ag" is 21. Reproduce it. */
void nd_textinput_draw(nd_textinput *t, bool blink_state);

/* Does NO drawing. */
nd_widget_result nd_textinput_handle_key(nd_textinput *t, int32_t key);

/* Returns the text pointer on confirm, NULL on cancel.
 *
 * THE CURSOR DOES NOT ACTUALLY BLINK. The Python checks its 0.5 s timer only
 * after wait_for_key() returns, and that blocks -- so an idle field never
 * blinks. A C loop with a poll() timeout WOULD blink and would then differ
 * from the golden frames. Keep the blocking wait. */
const char *nd_textinput_show(nd_textinput *t);

/* ================================================================== *
 * 9. TextInputLong -- the message composer
 * ================================================================== */

/* Called when Clear is pressed on an already-empty field. */
typedef void (*nd_empty_backspace_fn)(void *ctx);

typedef struct {
    nd_ui *ui;
    const char *title;
    char *text;
    size_t cap;
    size_t cursor; /* BYTE offset, not a character index */
    nd_t9_engine t9;
    nd_predictive predict;
    const nd_font *font;      /* font_s, 14 px */
    int32_t text_area_top;    /* 40  */
    int32_t text_area_bottom; /* 141 */
    nd_empty_backspace_fn on_empty_backspace;
    void *on_empty_ctx;
} nd_textlong;

nd_err nd_textlong_init(nd_textlong *t, nd_ui *ui, const char *title, char *text_buf, size_t cap,
                        const char *initial, nd_t9_filter filter);

void nd_textlong_set_on_empty_backspace(nd_textlong *t, nd_empty_backspace_fn fn, void *ctx);

void nd_textlong_draw(nd_textlong *t, bool blink_state);
nd_widget_result nd_textlong_handle_key(nd_textlong *t, int32_t key);

const char *nd_textlong_get_text(const nd_textlong *t);
nd_err nd_textlong_set_text(nd_textlong *t, const char *s);
void nd_textlong_clear_text(nd_textlong *t);

/* THERE IS DELIBERATELY NO nd_textlong_show(). The composing loop lives in the
 * Messages app, which owns the blink timer and the softkey. */

/* ================================================================== *
 * 10. MessageDialog -- the full-screen warning
 * ================================================================== */

#define ND_DIALOG_KEYS_MAX 8

typedef struct {
    nd_ui *ui;
    const char *message;
    const char *title;     /* may be NULL */
    const char *icon_path; /* NULL means the warning triangle */
    const char *button_text;
    int32_t accept_keys[ND_DIALOG_KEYS_MAX];
    size_t n_accept;
    int32_t cancel_keys[ND_DIALOG_KEYS_MAX];
    size_t n_cancel;
    int32_t margin; /* 8 */
} nd_msgdialog;

/* Passing icon_path NULL or "" both give the warning triangle; there is no way
 * to ask for no icon short of a path that fails to load. Pass n_cancel 0 for
 * an un-cancellable notice, which is what the low-battery shutdown does. */
void nd_msgdialog_init(nd_msgdialog *d, nd_ui *ui, const char *message);
void nd_msgdialog_set_title(nd_msgdialog *d, const char *title);
void nd_msgdialog_set_icon(nd_msgdialog *d, const char *icon_path);
void nd_msgdialog_set_button(nd_msgdialog *d, const char *button_text);
void nd_msgdialog_set_keys(nd_msgdialog *d, const int32_t *accept, size_t n_accept,
                           const int32_t *cancel, size_t n_cancel);

/* Draw only, no key loop. The low-battery shutdown uses this. */
void nd_msgdialog_render(nd_msgdialog *d);

/* Draw, then return the key that dismissed it. Callers compare against
 * ND_KEY_ENTER to tell Yes from No. Any other key is ignored with no redraw.
 *
 * When the message needs more than two lines at 20 px the dialog switches to
 * the 14 px left-aligned paragraph look; two lines or fewer keep the centred
 * Nokia alert look. When it still does not fit, the last line gets " …"
 * appended -- U+2026, WHICH THIS FONT CANNOT DRAW. It renders as nothing plus
 * 8 px of advance. Reproduce the codepoint; "..." would change the pixels. */
int32_t nd_msgdialog_show(nd_msgdialog *d);

/* ================================================================== *
 * 11. TextScroller -- the help reader
 * ================================================================== */

#define ND_SCROLLER_MAX_LINES 128

typedef struct {
    nd_ui *ui;
    const char *text;
    const char *more_text; /* "More" */
    const char *back_text; /* "Back" */
    const nd_font *font;   /* font_n, 20 px */
    int32_t margin;        /* 10 */
    int32_t top;           /* 8  */
    size_t page;
} nd_scroller;

void nd_scroller_init(nd_scroller *s, nd_ui *ui, const char *text, const char *more_text,
                      const char *back_text);

/* Repaginates from scratch, draws, and returns true when this is the last
 * page. The softkey update at the end is what PRESENTS the frame -- there is
 * no separate present. A blank source line costs 8 px, not a full 25. */
bool nd_scroller_draw(nd_scroller *s);
void nd_scroller_show(nd_scroller *s);

/* Exposed because two existing tests assert on the pagination directly. */
size_t nd_scroller_paginate(const nd_scroller *s, size_t *line_h_out);

/* ================================================================== *
 * 12. InfoScreen -- a centred label with a big number under it
 * ================================================================== */

/* value may be NULL, which means "no value" and re-centres the title alone.
 * An empty string is NOT the same as NULL: "0" renders as "0". */
int32_t nd_infoscreen_show(nd_ui *ui, const char *title, const char *value,
                           const char *softkey_text);

/* ================================================================== *
 * 13. ProgressScreen -- step name, bar, percentage
 * ================================================================== */

#define ND_PROGRESS_BAR_HEIGHT 14
#define ND_PROGRESS_BAR_MARGIN 20
#define ND_PROGRESS_INSET      2

/* Right-aligned text beside the percentage, e.g. "5.6 of 12.4 MB". */
typedef void (*nd_progress_detail_fn)(void *ctx, int64_t done, int64_t total, char *out,
                                      size_t out_sz);

typedef struct {
    nd_ui *ui;
    const char *step;
    const char *header; /* may be NULL */
    const char *hint;   /* may be NULL */
    nd_progress_detail_fn detail;
    void *detail_ctx;

    /* Computed once at init and read by the tests. */
    nd_rect header_box; /* (0, 4, 240, 19)    */
    nd_rect label_box;  /* (0, 44, 240, 65)   */
    nd_rect bar_box;    /* (20, 79, 220, 93)  */
    nd_rect status_box; /* (20, 102, 220, 117)*/
    nd_rect hint_box;   /* (0, 124, 240, 139) */
    int32_t divider_y;  /* 24 */

    int32_t percent; /* -1 == "nothing drawn yet" */
} nd_progress;

void nd_progress_init(nd_progress *p, nd_ui *ui, const char *step, const char *header,
                      const char *hint, nd_progress_detail_fn detail, void *detail_ctx);

/* Forces a repaint on the next draw. */
void nd_progress_set_step(nd_progress *p, const char *step);

/* Returns false AND DRAWS NOTHING when the percentage has not changed. The
 * copy loop calls this per 256 KB chunk; repainting each time would make the
 * update slower than the write. There is no show() -- the caller drives it. */
bool nd_progress_draw(nd_progress *p, int64_t done, int64_t total);

/* ================================================================== *
 * 14. DetailPage -- picture, title, rule, scrolling body
 * ================================================================== */

#define ND_DETAIL_MARGIN      10
#define ND_DETAIL_IMAGE_MAX   64
#define ND_DETAIL_MIN_IMAGE   40
#define ND_DETAIL_SCROLLBAR_W 8

/* The page is laid out once, into a bounded array of blocks. The Python builds
 * closures; a tagged union is the C equivalent and, unlike function pointers
 * with captured state, it can be inspected by a test. */
typedef enum {
    ND_BLOCK_TEXT = 0,
    ND_BLOCK_IMAGE,
    ND_BLOCK_HERO,
    ND_BLOCK_RULE,
    ND_BLOCK_GAP
} nd_block_kind;

#define ND_DETAIL_MAX_BLOCKS 256

typedef struct {
    nd_block_kind kind;
    int32_t height;
    int32_t x; /* precomputed draw origin within the column */
    const nd_font *font;
    char text[ND_TEXT_LINE_MAX];
} nd_detail_block;

typedef struct {
    nd_ui *ui;
    const char *title;
    const char *subtitle;
    const char *body;
    const char *badge;
    const char *header;
    const char *softkey_text;
    int32_t accept_keys[ND_DIALOG_KEYS_MAX];
    size_t n_accept;
    int32_t cancel_keys[ND_DIALOG_KEYS_MAX];
    size_t n_cancel;

    const nd_image *image; /* borrowed, already shrunk to fit */

    nd_rect viewport;
    int32_t line_height; /* 18 */
    int32_t content_height;
    int32_t body_top;
    int32_t offset;
    bool scrollable;

    nd_detail_block *blocks; /* heap: the only widget that needs it */
    size_t n_blocks;
} nd_detailpage;

/* Lays the page out. THE ONLY WIDGET THAT ALLOCATES -- release with
 * nd_detailpage_free(). image may be a path (loaded through the cache and
 * capped at 64 px) or NULL. */
nd_err nd_detailpage_init(nd_detailpage *p, nd_ui *ui, const char *title, const char *subtitle,
                          const char *body, const char *image_path, const char *badge,
                          const char *header, const char *softkey_text);
void nd_detailpage_free(nd_detailpage *p);

/* Draws into ui->scratch and blits the column, so scrolled text is clipped by
 * construction. A block that would be SLICED BY THE BOTTOM EDGE is skipped
 * entirely -- half a line at the fold reads as a bug. */
void nd_detailpage_draw(nd_detailpage *p);

int32_t nd_detailpage_max_offset(const nd_detailpage *p);

/* Scrolls by one line_height. Returns false, and does not redraw, when the
 * offset would not change -- so holding Down at the bottom is silent. */
bool nd_detailpage_handle_key(nd_detailpage *p, int32_t key);

/* Returns the key that dismissed the page. */
int32_t nd_detailpage_show(nd_detailpage *p);

/* ================================================================== *
 * The T9 mode indicator (drawn by both text widgets)
 * ================================================================== */

#define ND_T9_PENCIL_GAP 4

/* NULL-safe: returns 0 and sets nothing when there is no matrix keypad, which
 * is why QEMU never shows the indicator.
 * In predictive mode the label is "abc" with a pencil in front of it and the
 * total width is pencil + 4 + width("abc"); in the other three modes the label
 * is the mode name and there is no pencil. */
int32_t nd_t9ind_size(const nd_ui *ui, const nd_t9_engine *t9, const char **label_out,
                      int32_t *pencil_out);

/* Draws with the indicator's RIGHT EDGE at `right`. Returns the width used. */
int32_t nd_t9ind_draw(nd_ui *ui, int32_t right, int32_t y, const nd_t9_engine *t9);

/* Plotted PER PIXEL, deliberately not with polygon(): at the ~15 px this
 * renders at, a polygon's edges land wherever rounding puts them and the
 * barrel comes out either a hairline or twice the weight of the font beside
 * it. Uses nd_round_half_even() twice -- C's round() gives a different barrel
 * width at three of the sizes this is called with. */
void nd_draw_pencil(struct nd_draw *d, int32_t x, int32_t y, int32_t size, nd_color c);

/* ================================================================== *
 * The Dialer screens -- CORE PROCESS ONLY, never linked into an app
 * ================================================================== */

/* draw_call_screen does NOT present; the caller does. show_calling presents
 * TWICE per 0.25 s frame (once for the screen, once inside the softkey
 * update), which is visible as a two-stage repaint. Keep it. */
void nd_dialer_draw_call(nd_ui *ui, const char *number, const char *name);
void nd_dialer_show_calling(nd_ui *ui, const char *number, const char *name);

typedef enum { ND_CALL_ANSWERED = 0, ND_CALL_DECLINED, ND_CALL_GONE } nd_incoming_result;

void nd_dialer_draw_incoming(nd_ui *ui, const char *caller_text, bool blink_on);
nd_incoming_result nd_dialer_show_incoming(nd_ui *ui, const char *number, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* ND_WIDGETS_H_INCLUDED */
