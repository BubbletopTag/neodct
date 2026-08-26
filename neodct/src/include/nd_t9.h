/* nd_t9.h -- typing words on twelve keys.
 *
 * Two halves, and they are independent:
 *
 *   the ENGINE  pure logic. Multi-tap cycling, mode switching, and the digit
 *               string that predictive mode accumulates. No I/O, no
 *               allocation, and NO CLOCK OF ITS OWN -- the multi-tap timeout
 *               needs a monotonic clock and the tests inject a fake one.
 *
 *   the DICT    a binary search over /NeoDCT/System/core/t9.dict, roughly
 *               3 MB, WHICH IS NEVER LOADED INTO RAM. One open descriptor,
 *               about seventeen seeks per lookup. On a phone with 53 MB that
 *               is not an optimisation, it is the only way this feature can
 *               exist at all.
 */

#ifndef ND_T9_H_INCLUDED
#define ND_T9_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ND_T9_FILTER_ANY = 0, ND_T9_FILTER_LETTERS, ND_T9_FILTER_NUMBERS } nd_t9_filter;

typedef enum {
    ND_T9_MODE_WORD = 0, /* "word" -- predictive          */
    ND_T9_MODE_ABC,      /* "abc"                         */
    ND_T9_MODE_UPPER,    /* "ABC"                         */
    ND_T9_MODE_123       /* "123"                         */
} nd_t9_mode;

/* What one key press means. The widget layer switches on kind. */
typedef enum {
    ND_T9_OP_NONE = 0, /* the engine consumed it; nothing to do            */
    ND_T9_OP_APPEND,   /* add ch at the insertion point                    */
    ND_T9_OP_REPLACE,  /* replace the character before the point with ch   */
    ND_T9_OP_MODE,     /* the mode changed; redraw the indicator           */
    ND_T9_OP_WORD,     /* predictive: re-predict from digits               */
    ND_T9_OP_NEXT      /* predictive: show the next candidate              */
} nd_t9_opkind;

typedef struct {
    nd_t9_opkind kind;
    char ch;            /* APPEND / REPLACE                              */
    nd_t9_mode mode;    /* MODE                                          */
    const char *digits; /* WORD / NEXT -- points into the engine         */
} nd_t9_op;

/* Monotonic seconds. Injected so the multi-tap timeout is testable without
 * sleeping. Pass NULL to nd_t9_engine_init for the real clock. */
typedef double (*nd_clock_fn)(void *ctx);

/* The digit string predictive mode accumulates. The longest dictionary key is
 * 12, but nothing in the Python stops a user holding 2 and accumulating an
 * arbitrarily long sequence -- so the C caps it at 32 and stops appending.
 * suggest() returns nothing past 12 anyway. Deviation recorded in
 * OPEN-QUESTIONS.md. */
#define ND_T9_DIGITS_MAX 32

/* Sized so the whole engine can be embedded in a widget struct with no
 * allocation. Fields are private; use the accessors. */
typedef struct nd_t9_engine {
    nd_t9_filter filter;
    nd_t9_mode mode;
    nd_t9_mode modes[4];
    size_t n_modes;
    size_t mode_index;
    double timeout_s;
    nd_clock_fn clock;
    void *clock_ctx;
    int32_t last_code;
    size_t cycle_index;
    double last_press_at;
    char word_digits[ND_T9_DIGITS_MAX + 1];
} nd_t9_engine;

/* timeout_s is the multi-tap window; pass 0 for the Python's default. */
nd_err nd_t9_engine_init(nd_t9_engine *e, nd_t9_filter f, double timeout_s, nd_clock_fn clk,
                         void *clk_ctx);

nd_t9_op nd_t9_engine_press(nd_t9_engine *e, int32_t code);

nd_t9_mode nd_t9_engine_mode(const nd_t9_engine *e);
const nd_t9_mode *nd_t9_engine_modes(const nd_t9_engine *e, size_t *count);
nd_t9_mode nd_t9_engine_set_mode_index(nd_t9_engine *e, size_t index);

/* The accumulated predictive digits, "" when there are none. */
const char *nd_t9_engine_word_digits(const nd_t9_engine *e);

/* Drop the last digit. Returns the remaining digits, or NULL when there was
 * nothing to drop. This is why Clear on a predictive word removes A TYPED
 * DIGIT rather than a guessed letter: "good" becomes "inn" (the guess for
 * "466"), not "goo". */
const char *nd_t9_engine_pop_word_digit(nd_t9_engine *e);

void nd_t9_engine_clear_word(nd_t9_engine *e);
void nd_t9_engine_reset(nd_t9_engine *e);

bool nd_t9_char_allowed(char c, nd_t9_filter f);
const char *nd_t9_mode_label(nd_t9_mode m); /* "word" / "abc" / "ABC" / "123" */

/* ------------------------------------------------------------------ *
 * The dictionary
 * ------------------------------------------------------------------ */

#define ND_T9_WORD_MAX        16 /* 12 bytes plus NUL plus slack */
#define ND_T9_MAX_SUGGESTIONS 8
#define ND_T9_MIN_PREFIX      2

typedef struct nd_t9_dict nd_t9_dict;

/* NULL is NOT an error -- an image without the dictionary simply has no
 * predictions, and the keypad must still work. */
nd_t9_dict *nd_t9_dict_open(const char *path);
void nd_t9_dict_close(nd_t9_dict *d);
bool nd_t9_dict_available(const nd_t9_dict *d);

/* Process-wide, opened lazily on first use, never closed. One descriptor for
 * the life of the process is the whole memory cost. */
nd_t9_dict *nd_t9_dict_shared(void);

/* The digits that would type `word`, e.g. "hello" -> "43556".
 * false when the word contains something untypeable. */
bool nd_t9_digits_for(const char *word, char *out, size_t out_sz);

/* Candidates for a digit string, best first. Digits must all be in "23456789"
 * and there must be at least ND_T9_MIN_PREFIX of them, or the result is zero.
 *
 * NO ALLOCATION: the caller supplies the array, so a char[8][16] on the stack
 * -- 128 bytes -- is the entire cost of a lookup. */
size_t nd_t9_dict_suggest(nd_t9_dict *d, const char *digits, char out[][ND_T9_WORD_MAX],
                          size_t limit);

/* ------------------------------------------------------------------ *
 * The shell and browser bridges
 * ------------------------------------------------------------------ */

struct nd_input;
struct nd_uinput_kbd;

typedef enum { ND_BRIDGE_SHELL = 0, ND_BRIDGE_BROWSER } nd_bridge_kind;
typedef struct nd_t9_bridge nd_t9_bridge;

/* Reads the keypad and types into a uinput device, so LinuxShell and NetSurf
 * see an ordinary keyboard. NULL when there is no matrix keypad, which is the
 * correct no-op on a QEMU keyboard. */
nd_t9_bridge *nd_t9_bridge_start(nd_bridge_kind kind, struct nd_input *in,
                                 struct nd_uinput_kbd *kbd);
/* Exposed so the bridge's logic is testable without a device. */
void nd_t9_bridge_handle_code(nd_t9_bridge *b, int32_t code);
void nd_t9_bridge_stop(nd_t9_bridge *b); /* joins, closes the keyboard */

#ifdef __cplusplus
}
#endif

#endif /* ND_T9_H_INCLUDED */
