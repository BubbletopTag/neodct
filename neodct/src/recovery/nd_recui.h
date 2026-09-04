/* nd_recui.h -- the contract between nd_recui.c, nd_recdraw.c and
 * nd_recinput.c, and nothing else.
 *
 * ============ THIS PROGRAM IS OUTSIDE libneodct, DELIBERATELY ============
 *
 * nd-recui runs inside the initramfs. /NeoDCT/System is the filesystem
 * recovery exists because it could not be mounted, so libneodct, font.ttf,
 * freetype, sqlite and every nd_*.h are by definition unreachable. The
 * Makefile builds this directory WITHOUT -Iinclude for exactly that reason:
 * a stray `#include "nd_ui.h"` has to fail to compile rather than quietly
 * link a program the initramfs cannot run.
 *
 * Two consequences, both departures from CODING-STANDARDS.md that are
 * confined to this directory and are named here so nobody has to guess:
 *
 *   * No nd_err, no nd_log. Section 3's convention is for code that links
 *     the library. Here a failure is an int return and an fprintf to stderr,
 *     which lands on /dev/console beside the shell's own log() output.
 *   * The constants below are COPIED from headers this program may not
 *     include, each with the header named. That is the same move nd_evdev.c
 *     already makes for struct input_event and EVIOCGNAME, for the same
 *     reason -- and the unit test walks the sixteen key codes against
 *     nd_keycodes.h to make sure the copies stay true.
 *
 * ============ THE UI IS ONE BIT DEEP, ON PURPOSE ============
 *
 * QEMU's framebuffer is B G R x and the phone's vfb is R G B x (nd_fb.h
 * documents the disagreement at length). Black and white are identical under
 * any channel permutation, and so is grey with three equal components, so
 * drawing only those three deletes the entire class of bug -- and matches
 * /splash.raw and /bootlogo.raw, which are already two-colour.
 *
 * ============ NO HEAP AFTER STARTUP ============
 *
 * One mmap of fb0 and static arrays for everything else. -Wvla is on and
 * CODING-STANDARDS section 1.5 forbids an input-sized array, so a menu longer
 * than ND_RECUI_MAX_ITEMS is truncated with a note on the console.
 */

#ifndef ND_RECUI_H_INCLUDED
#define ND_RECUI_H_INCLUDED

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ *
 * Exit codes -- the shell reads these
 * ------------------------------------------------------------------ */

/* "I have no usable input device." ndsys-recovery.sh falls back to its own
 * tty menu, which is today's behaviour, rather than drawing a pretty menu
 * nobody can move. This is the whole error strategy. */
#define ND_RECUI_EXIT_NO_INPUT 2

/* ------------------------------------------------------------------ *
 * Geometry -- copied from nd_ui.h, which this may not include
 * ------------------------------------------------------------------ */

#define ND_RECUI_W              240 /* ND_UI_W                             */
#define ND_RECUI_H              175 /* ND_UI_H                             */
#define ND_RECUI_CONTENT_BOTTOM 145 /* ND_UI_H - ND_SOFTKEY_H              */

/* nd_widgets.h */
#define ND_RECUI_BAR_HEIGHT 14
#define ND_RECUI_BAR_MARGIN 20
#define ND_RECUI_BAR_INSET  2

#define ND_RECUI_MAX_ITEMS    16
#define ND_RECUI_ITEM_MAX     64
#define ND_RECUI_MAX_MSG_LINE 8

/* ------------------------------------------------------------------ *
 * Key codes -- copied from nd_keycodes.h. NeoDCT key codes ARE evdev key
 * codes, which is why nothing here translates.
 *
 * SIXTEEN KEYS. There is no left and no right on this phone; the two codes
 * exist in nd_keycodes.h and reach only a development QWERTY keyboard.
 * Nothing in recovery may depend on them.
 * ------------------------------------------------------------------ */

#define ND_RECKEY_NONE  (-1)
#define ND_RECKEY_CLEAR 14 /* KEY_BACKSPACE; "clear" and "back"         */
#define ND_RECKEY_ENTER 28 /* KEY_ENTER; the NaviKey                    */
#define ND_RECKEY_STAR  42 /* KEY_LEFTSHIFT, abused as '*'              */
#define ND_RECKEY_HASH  43 /* KEY_BACKSLASH, abused as '#'              */
#define ND_RECKEY_UP    103
#define ND_RECKEY_DOWN  108
#define ND_RECKEY_1     2 /* .. ND_RECKEY_9 == 10                      */
#define ND_RECKEY_9     10
#define ND_RECKEY_0     11 /* after 9, in evdev order                   */

/* ------------------------------------------------------------------ *
 * The framebuffer
 * ------------------------------------------------------------------ */

typedef enum {
    ND_RECCOL_BLACK = 0,
    ND_RECCOL_WHITE = 1,
    /* nd_vlist.c: "the only grey pixels in the entire framework". Three
     * equal components, so a channel permutation still leaves it grey --
     * which is why keeping it costs nothing against the one-bit rule. */
    ND_RECCOL_GREY = 2
} nd_reccolour;

typedef struct {
    int fd;      /* -1 for a memory target                             */
    uint8_t *px; /* the mapping, or a malloc for a memory target       */
    size_t px_len;
    int32_t w;
    int32_t h;
    int32_t bpp;   /* 32 or 16; anything else is refused at open         */
    size_t stride; /* line_length, with nd_fb.h's "0 means compute it"   */
    bool owns_mem; /* free() rather than munmap()                        */
} nd_recfb;

/* /dev/fb0: two ioctls, an mmap, and a zero. Returns 0, or -1 with a reason
 * on stderr. A bpp that is neither 32 nor 16 is a refusal, not a guess. */
int nd_recfb_open(nd_recfb *fb, const char *path);

/* A plain buffer with the panel's geometry, for the host tests and for the
 * screenshot path. Same drawing code, no device. */
int nd_recfb_open_mem(nd_recfb *fb, int32_t w, int32_t h, int32_t bpp);
void nd_recfb_close(nd_recfb *fb);

/* Read one pixel back as a colour index. Tests and the PNG dump only; the
 * draw path never reads. Off-canvas reads report black. */
nd_reccolour nd_recfb_get(const nd_recfb *fb, int32_t x, int32_t y);

/* ------------------------------------------------------------------ *
 * Drawing. Rects are INCLUSIVE of both corners and clip silently, which is
 * what nd_draw.h does and what every ported coordinate assumes.
 * ------------------------------------------------------------------ */

void nd_recdraw_rect(nd_recfb *fb, int32_t x0, int32_t y0, int32_t x1, int32_t y1, nd_reccolour c);

/* Rows y0..y1 inclusive, full width. The recovery equivalent of
 * nd_ui_paint_chrome_content(): rows 0..content_bottom only, so a caller can
 * leave the strip below alone. */
void nd_recdraw_clear(nd_recfb *fb, int32_t y0, int32_t y1, nd_reccolour c);

/* Blit a 240x175 XRGB8888 blob -- /splash.raw's format. Converts to 16bpp if
 * that is what the driver gave us. Returns 0, or -1. */
int nd_recdraw_blit_raw(nd_recfb *fb, const char *path);

/* ------------------------------------------------------------------ *
 * Text
 * ------------------------------------------------------------------ */

/* Which of the two generated tables. 18 px carries the title and the menu
 * items; 14 px the heading, the reading and filenames. */
typedef enum { ND_RECFONT_LARGE = 0, ND_RECFONT_SMALL = 1 } nd_recfontsize;

/* y is the ASCENDER line -- Pillow's "la" anchor, the same y nd_draw_text()
 * takes -- so a label's visible top is a couple of pixels below it. */
void nd_recdraw_text(nd_recfb *fb, int32_t x, int32_t y, const char *s, nd_recfontsize f,
                     nd_reccolour c);

/* nd_text_bbox()'s arithmetic: the width is the PEN (the sum of advances, so
 * a trailing space counts), and the height is the ink box measured from a box
 * that starts collapsed on the baseline. Pure; the unit test checks it
 * against tests/golden/font/fontref.json. */
void nd_recdraw_text_size(nd_recfontsize f, const char *s, int32_t *w, int32_t *h);

/* Longest prefix of s that fits in `room` pixels, copied into out. Hard
 * truncation, not an ellipsis: at 14 px on a 240 px panel a package name has
 * no room to spend on "...". Returns the pixel width of what it kept. */
int32_t nd_recdraw_text_fit(char *out, size_t out_sz, const char *s, nd_recfontsize f,
                            int32_t room);

/* ------------------------------------------------------------------ *
 * The vertical list -- nd_vlist_draw()'s arithmetic, kept identical
 * ------------------------------------------------------------------ */

typedef struct {
    int32_t header_y;       /* the divider row                             */
    int32_t y_start;        /* first row's top                             */
    int32_t line_height;    /* row pitch                                   */
    int32_t item_height;    /* the white bar's height                      */
    int32_t max_lines;      /* rows that fit                               */
    int32_t bar_x;          /* the scrollbar column                        */
    int32_t selected_right; /* the white bar stops short of the scrollbar  */
    int32_t track_top;
    int32_t track_bottom;
} nd_reclist_metrics;

/* On this panel: header_y 30, y_start 40, line_height 33, item_height 29,
 * max_lines 3, bar_x 235, selected_right 225 -- rows at 40, 73 and 106.
 * Derived, not hard-coded, so a taller panel still lays out. */
void nd_reclist_metrics_of(int32_t width, int32_t content_bottom, nd_reclist_metrics *m);

/* Slide the window so `selected` is inside it, then clamp it to the end of
 * the list. Returns the new window start. Pure. */
size_t nd_reclist_window(size_t selected, size_t window_start, size_t n_items, size_t max_lines);

/* trunc(track_top + selected * (track_bottom - track_top) / (n_items - 1)).
 * A FLOAT that truncates -- rounding moves the notch on most list lengths. */
int32_t nd_reclist_notch_y(const nd_reclist_metrics *m, size_t selected, size_t n_items);

/* One frame. window_start is in/out so the caller keeps its place. */
void nd_reclist_draw(nd_recfb *fb, const char *title, const char *const *items, size_t n_items,
                     size_t selected, size_t *window_start);

/* Up, Down, digits 1..9, Enter and Clear, exactly as nd_vlist_handle_key()
 * treats them. Returns the chosen 0-based index, or one of these. */
#define ND_RECLIST_CONTINUE (-1)
#define ND_RECLIST_BACK     (-2)
int32_t nd_reclist_key(int32_t key, size_t n_items, size_t *selected);

/* ------------------------------------------------------------------ *
 * The progress screen -- nd_progress_draw()'s five boxes
 * ------------------------------------------------------------------ */

typedef struct {
    int32_t bar_x0, bar_y0, bar_x1, bar_y1;
    int32_t label_y;  /* the step name's ascender line, above the bar      */
    int32_t status_y; /* the reading's ascender line, below it             */
} nd_recprogress_metrics;

void nd_recprogress_metrics_of(int32_t width, int32_t content_bottom, nd_recprogress_metrics *m);

/* int(done * 100 / total), truncated toward zero and clamped to 0..100.
 * total == 0 means "done", not a divide by zero. */
int32_t nd_recprogress_percent(int64_t done, int64_t total);

/* trunc(span * percent / 100). The inner fill's width. */
int32_t nd_recprogress_filled(int32_t span, int32_t percent);

/* "48.0M", "512K", "37" -- the right-hand detail. Two significant figures is
 * all 220 px of bar has room to report next to a percentage. */
void nd_recprogress_human(char *out, size_t out_sz, int64_t bytes);

typedef struct {
    nd_recfb *fb;
    nd_recprogress_metrics m;
    const char *step;
    const char *header;
    int32_t percent; /* -1 = nothing drawn yet; Python's None */
} nd_recprogress;

void nd_recprogress_init(nd_recprogress *p, nd_recfb *fb, const char *step, const char *header);

/* Paints NOTHING and returns false when the whole percentage has not moved.
 * That gate is what stops the bar being slower than the write it reports on:
 * fed 64 KB at a time over 48 MB it is the difference between 750 redraws
 * and 100. */
bool nd_recprogress_draw(nd_recprogress *p, int64_t done, int64_t total);

/* ------------------------------------------------------------------ *
 * The message page
 * ------------------------------------------------------------------ */

void nd_recmessage_draw(nd_recfb *fb, const char *const *lines, size_t n_lines);

/* ------------------------------------------------------------------ *
 * The yes/no page
 * ------------------------------------------------------------------ *
 *
 * `selected` is 0 for yes, 1 for no, and NEGATIVE for "neither". Recovery's
 * two destructive questions open with neither lit, so that a stray Enter on
 * "WIPE SYSTEM?" cannot answer it at all. The tty menu defaults to no; this
 * is a deliberate divergence and the reason is the question.
 *
 * The question WRAPS at 14 px, because it is a sentence rather than a label:
 * "WIPE USER DATA? Contacts, messages and settings will be erased." is 500 px
 * at 18 px and there is no size at which it is one line on a 240 px panel.
 *
 * There is NO title and no divider here, unlike every other screen. Two rows
 * of options take 62 of the 146 content rows, which leaves room for four
 * wrapped lines and no more -- and a title costs the fourth. Measured against
 * the real strings, three lines cuts "WIPE USER DATA? Contacts, messages and
 * settings will be erased." off after "and", which is precisely the half of
 * the sentence somebody needs. The question IS the screen.
 */
void nd_recconfirm_draw(nd_recfb *fb, const char *question, int selected);

/* ------------------------------------------------------------------ *
 * The VT -- KD_GRAPHICS is required, not optional
 * ------------------------------------------------------------------ *
 *
 * With the VT in its default mode the kernel ECHOES every key pressed while
 * nd-recui runs onto tty1, and fbcon paints that text over our framebuffer.
 * Failure to acquire the VT is ignored: a build with no VT loses nothing.
 */
void nd_recvt_graphics(void);
void nd_recvt_text(void);

/* ------------------------------------------------------------------ *
 * Input
 * ------------------------------------------------------------------ */

#define ND_RECINPUT_MAX_EVDEV 8
#define ND_RECMATRIX_MAX_PINS 16 /* the expander has sixteen               */

/* nd_keypad.h: a membrane contact chatters on the way up, not on the way
 * down, so a release is debounced over this many scans and a press is
 * reported the instant it appears. */
#define ND_RECMATRIX_RELEASE_SCANS 3
#define ND_RECMATRIX_SETTLE_US     500

typedef struct {
    uint8_t row_pins[ND_RECMATRIX_MAX_PINS];
    size_t n_rows;
    uint8_t col_pins[ND_RECMATRIX_MAX_PINS];
    size_t n_cols;
    /* -1 where a position is unmapped. */
    int32_t code[ND_RECMATRIX_MAX_PINS][ND_RECMATRIX_MAX_PINS];
    int i2c_bus;
    int i2c_addr;
    bool any_key;
} nd_reckeymap;

/* "navikey" -> 28, "num_0" -> 11, and the fourteen others. -1 for a name
 * this phone does not have -- including "left" and "right", which
 * nd_keycode_for_name() does accept and which must never reach a menu here.
 * `len` so the caller can pass a slice of the JSON without copying. */
int32_t nd_reckey_for_name(const char *name, size_t len);

/* Four targeted extractions over ~2 KB of keymap.json: row_pins, col_pins,
 * i2c_bus/i2c_addr and by_matrix. No JSON parser -- there is none in an
 * initramfs, and this is the same trade recovery_manifest_field() already
 * takes, with the advantage of being whitespace-independent.
 *
 * by_matrix rather than the nested "keys" object because it is already the
 * flat "row,col" -> name map this needs. Note "row_pin" (singular, inside
 * "keys") cannot collide with "row_pins".
 *
 * Forgiving exactly as nd_keymap.c is: an unknown name or an out-of-range
 * position is skipped in silence, because a keymap missing the '7' key still
 * rescues a phone. Returns 0 when at least one key was recognised, -1
 * otherwise. */
int nd_reckeymap_parse(const char *text, nd_reckeymap *out);

/* Read and parse the file. -1 if it is absent or yields no keys. */
int nd_reckeymap_load(const char *path, nd_reckeymap *out);

typedef struct {
    int fd; /* -1 when there is no chip                                */
    bool owns_fd;
    const nd_reckeymap *map;
    /* -1 = not held, else consecutive scans it has been missing from.    */
    int8_t held[ND_RECMATRIX_MAX_PINS][ND_RECMATRIX_MAX_PINS];
} nd_recmatrix;

/* Open /dev/i2c-<bus> and claim the address. Returns 0, or -1. */
int nd_recmatrix_open(nd_recmatrix *mx, const nd_reckeymap *map);

/* Tests: the same thing over a descriptor the caller supplies -- one end of
 * a socketpair standing in for the chip, which is the hook nd_keypad.h
 * documents and test_keypad.c already uses. No ioctl is issued. */
int nd_recmatrix_attach(nd_recmatrix *mx, const nd_reckeymap *map, int fd);

/* One full pass over every row. Returns a key code for a NEW press, or
 * ND_RECKEY_NONE. Unlike nd_matrix_scan_once() there is no pending queue and
 * no rollover: a menu needs one key at a time, and nd_matrix.c's own comment
 * says the queue exists for games. */
int32_t nd_recmatrix_scan(nd_recmatrix *mx);
void nd_recmatrix_close(nd_recmatrix *mx);

typedef struct {
    int evdev[ND_RECINPUT_MAX_EVDEV];
    size_t n_evdev;
    nd_recmatrix matrix;
    bool have_matrix;
    nd_reckeymap map;
} nd_recinput;

/* Both backends are probed and BOTH are polled if both open, so a phone with
 * a dev keyboard plugged in keeps working. Returns 0 when at least one
 * opened, -1 when neither did -- which is what makes the caller exit 2. */
int nd_recinput_open(nd_recinput *in, const char *keymap_path);

/* Blocks until a key arrives. ND_RECKEY_NONE only if every device died. */
int32_t nd_recinput_wait(nd_recinput *in);
void nd_recinput_close(nd_recinput *in);

/* Decode one struct input_event out of a read buffer, in BOTH the 16-byte
 * and the 24-byte layouts, keyed off the read size exactly as nd_evdev.c
 * does. Returns the key code for a press (value 1) or a kernel autorepeat
 * (value 2), else ND_RECKEY_NONE. Pure, and the reason it is exposed. */
int32_t nd_recevdev_decode(const uint8_t *buf, size_t n);

#endif /* ND_RECUI_H_INCLUDED */
