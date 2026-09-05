/* bibleapp.h -- what main.c and reader.c agree on.
 *
 * Split from bible.h so that the pack reader and the paraphraser stay
 * testable without an nd_ui: test_bible.c links against a built app.so and
 * exercises them with no framebuffer at all.
 */

#ifndef ND_BIBLEAPP_H_INCLUDED
#define ND_BIBLEAPP_H_INCLUDED

#include "nd_paths.h"
#include "nd_props.h"
#include "nd_types.h"
#include "nd_ui.h"

#include "bible.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The manifest's id, and the breadcrumb every screen shows in the corner. */
#define BIBLE_APP_ID 13

/* Where a pack may live, in the order they are tried.
 *
 * The app's own directory first, which is where the shipped WEB pack lands as
 * part of the read-only rootfs, and then the user partition, which is the
 * ONLY writable storage on the phone (AGENTS.md: "/" is squashfs under
 * dm-verity). A translation that cannot be redistributed, packed by whoever
 * owns a copy of it with tools/mkbible.py, goes in the second place -- and
 * wins, because a pack somebody deliberately copied onto the device is more
 * likely to be the one they want than the one that shipped. */
#define BIBLE_USER_DIR  ND_PATH_USER "/Bible"
#define BIBLE_STATE_REL "/state.prop"

/* Bookmarks are stored one per key so that adding one is a set() and never a
 * re-serialisation of a list. */
#define BIBLE_MAX_BOOKMARKS 20

/* Search stops here. The list is a screen at a time and nobody pages through
 * nine hundred hits; stopping also bounds the time the scan can run. */
#define BIBLE_MAX_HITS 60

/* One verse, rendered. Gen Z expands the text, and the longest verse in the
 * shipped pack (Esther 8:9) is a little over 500 bytes, so this is roughly
 * three times the worst case rather than a number chosen to be round. */
#define BIBLE_VERSE_MAX 2048

typedef struct {
    nd_ui *ui;
    nd_bible *b;

    bool genz;

    /* Where the reader was last left, so "Continue" has somewhere to go.
     * book and chapter are 0-based indices; verse is the 1-based number a
     * human reads, which is the one convention this app does not unify --
     * see nd_bible_verse(). */
    size_t book;
    size_t chapter;
    size_t verse;
    bool have_pos;

    /* /NeoDCT/User/Bible/state.prop, read at start and written on exit.
     * Deliberately NOT settings.prop: the core owns that file and rewrites it
     * whole, so an app writing into it races the core for no benefit. */
    nd_props *state;
    bool state_dirty;
    char state_path[ND_PATH_MAX];
} bible_app;

/* ------------------------------------------------------------------ *
 * reader.c
 * ------------------------------------------------------------------ */

/* The chapter view. Returns when the user backs out of it. verse may be 0
 * for "the top of the chapter"; anything else scrolls that verse into view.
 * Updates a->book/chapter/verse as the reader moves, so the caller does not
 * have to. */
void bible_read(bible_app *a, size_t book, size_t chapter, size_t verse);

/* The number entry screen, used for both "which chapter" and "which verse".
 * Up/Right and Down/Left step; digits type; Clear backspaces and then leaves.
 * Returns the chosen value in [lo, hi], or -1 for Back. */
int32_t bible_pick_number(nd_ui *ui, const char *title, int32_t lo, int32_t hi, int32_t cur);

/* The verse as it should be drawn: the pack's own text, or the paraphrase,
 * depending on a->genz. Never NULL. */
const char *bible_verse_text(bible_app *a, size_t book, size_t chapter, size_t verse, char *buf,
                             size_t buf_sz);

/* ------------------------------------------------------------------ *
 * main.c -- shared with reader.c, and with the unit test
 * ------------------------------------------------------------------ */

/* "John 3:16", "Joh 3", "1 Cor 13:4", "gen1:1". Whitespace between the parts
 * is optional and the chapter and verse are both optional; what is missing
 * comes back as 0 in the 1-based numbering the caller then converts.
 *
 * Returns true and writes *book (0-based), *chapter (1-based, 0 if absent)
 * and *verse (1-based, 0 if absent). False when the book cannot be named. */
bool bible_parse_ref(const nd_bible *b, const char *s, size_t *book, size_t *chapter,
                     size_t *verse);

/* "John 3:16" into out. verse 0 gives "John 3"; chapter 0 gives "John". */
void bible_format_ref(const nd_bible *b, size_t book, size_t chapter, size_t verse, char *out,
                      size_t out_sz);

/* Remember where the reader is, for "Continue" and for the next launch. */
void bible_remember(bible_app *a, size_t book, size_t chapter, size_t verse);

/* Append a bookmark for the current position. Silently does nothing once
 * BIBLE_MAX_BOOKMARKS are stored -- the caller says so on screen. Returns
 * false when the list was full. */
bool bible_bookmark_add(bible_app *a, size_t book, size_t chapter, size_t verse);

#ifdef __cplusplus
}
#endif

#endif /* ND_BIBLEAPP_H_INCLUDED */
