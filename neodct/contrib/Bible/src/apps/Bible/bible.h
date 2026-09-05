/* bible.h -- the .ndb pack format, its reader, and the Gen Z paraphraser.
 *
 * Everything here is private to apps/Bible. It is deliberately NOT in
 * lib/: libneodct.so is mapped by every process on the phone, and a
 * scripture reader is not something the dialer should be paying for.
 *
 * ============ WHY A CUSTOM CONTAINER AT ALL ============
 *
 * The whole Bible is about 5 MB of text. The device has 64 MB of RAM and a
 * read-only squashfs root, so the two obvious options both lose:
 *
 *   - one flat .txt, read whole:   5 MB resident for a screen showing 8 lines
 *   - one .txt per chapter:        1,600-odd inodes, and squashfs pays a
 *                                  fragment for each
 *
 * So: one file, one chapter per zlib block, a fixed-size index in front. The
 * reader mallocs exactly one inflate buffer (max_raw, stamped by the packer)
 * and reuses it for every chapter for the life of the app. Opening John 3
 * costs one pread of ~2 KB and one inflate; the resident set never grows.
 *
 * zlib is not a new dependency -- lib/nd_package.c already inflates .ndsw
 * archives with it, so it is in NEODCT_DEPENDENCIES and in the image.
 *
 * ============ THE FORMAT ============
 *
 * All integers are LITTLE-ENDIAN and unaligned-safe (read byte by byte
 * through rd16/rd32). Both targets are little-endian -- aarch64 under QEMU
 * and the RV1103's Cortex-A7 -- but a pack is a file a user copies between
 * machines, so it gets a defined byte order rather than the host's.
 *
 *   header      80 bytes, see the ND_NDB_* offsets below
 *   book table  n_books   * 16 bytes
 *   chap table  n_chapters * 12 bytes
 *   string pool book display names, NUL-separated
 *   dictionary  up to 32 KB of preset zlib history (optional)
 *   blob        the deflated chapters, back to back
 *
 * ============ THE PRESET DICTIONARY ============
 *
 * Deflating 1,402 chapters separately means 1,402 compressors that each start
 * with an empty 32 KB window, so none of them knows that "and he said to him"
 * has occurred four hundred times already. Priming every one of them with the
 * same block of common phrasing costs 32 KB in the file and one
 * inflateSetDictionary() per chapter.
 *
 * Measured on the shipped WEB text: 2,026,131 bytes of chapter blob without
 * it, 1,727,653 with -- 14.7%. After paying the 32 KB the block itself costs,
 * the pack goes from 2,045,054 bytes to 1,779,344: 259 KB back, on a part
 * with 128 MB of NAND.
 *
 * mkbible.py builds the block by scoring every 2-to-6-word phrase in the
 * corpus by (length - 3) * (occurrences - 1), which is roughly the bytes
 * having it in the window saves, and filling 32 KB best-first. Best-LAST was
 * tried with the same phrases and the same scoring, on the theory that zlib
 * spends fewer bits on a nearer match; it came out 7.5% bigger. The
 * measurement decided it, not the theory.
 *
 * A pack without ND_NDB_F_DICT simply has dict_len == 0 and inflates without
 * one, so the flag is what makes this optional rather than a format break.
 *
 * A chapter inflates to its verses separated by '\n', with no trailing
 * newline. Verse n is line n-1. The packer guarantees the verse numbers in a
 * chapter are contiguous from 1, emitting an empty line for any the source
 * skips, so that indexing is arithmetic and never a search.
 *
 * ============ TRANSLATIONS AND THE LICENCE ============
 *
 * The pack that ships is the World English Bible, which is public domain.
 * Its own terms ask one thing: if you CHANGE the text, do not go on calling
 * the result the World English Bible. Gen Z mode changes the text, which is
 * why nd_genz() is a display filter that never writes back into the pack and
 * why the reader labels the mode "GEN Z" rather than "WEB" while it is on.
 * See ABOUT_TEXT in main.c, which says so on screen.
 *
 * neodct/tools/mkbible.py builds a pack from any verse-per-line text file, so
 * a translation that cannot be redistributed can still be packed by whoever
 * already has a copy of it. Nothing about the format is WEB-specific.
 */

#ifndef ND_BIBLE_H_INCLUDED
#define ND_BIBLE_H_INCLUDED

#include <stddef.h>
#include <stdint.h>

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ *
 * On-disk constants -- shared with tools/mkbible.py, which writes them
 * ------------------------------------------------------------------ */

#define ND_NDB_MAGIC   "NDBIBLE\x1a"
#define ND_NDB_MAGIC_N 8u
#define ND_NDB_VERSION 1u

#define ND_NDB_HDR_SZ   80u
#define ND_NDB_BOOK_SZ  16u
#define ND_NDB_CHAP_SZ  12u
#define ND_NDB_NAME_MAX 16u /* the translation label, NUL-padded */

/* The zlib window size, and therefore the most preset history that can
 * possibly be used. A pack claiming more is rejected. */
#define ND_NDB_DICT_MAX 32768u

/* flags */
#define ND_NDB_F_DEFLATE 0x0001u
#define ND_NDB_F_DICT    0x0002u

/* book.section */
#define ND_NDB_OT        0u
#define ND_NDB_NT        1u
#define ND_NDB_APOCRYPHA 2u

/* A refusal ceiling, not a format limit: a pack claiming more than this is
 * corrupt or hostile, and we would rather say so than malloc it. Psalm 119 is
 * the longest chapter in the shipped pack at just under 9 KB. */
#define ND_NDB_RAW_MAX (256u * 1024u)

/* ------------------------------------------------------------------ *
 * The reader
 * ------------------------------------------------------------------ */

typedef struct nd_bible nd_bible;

/* Opens and validates the index. The chapter blob is NOT read; the file stays
 * open for the life of the handle. path goes through nd_path_resolve(), so
 * NEODCT_ROOT applies as it does everywhere else. */
nd_err nd_bible_open(nd_bible **out, const char *path);
void nd_bible_close(nd_bible *b);

/* The translation label the packer stamped, e.g. "WEB". Never NULL. */
const char *nd_bible_translation(const nd_bible *b);

size_t nd_bible_book_count(const nd_bible *b);

/* "" for an out-of-range index rather than NULL, so a caller may draw the
 * result without checking. Indices are 0-based throughout; only the verse
 * and chapter NUMBERS a human reads are 1-based. */
const char *nd_bible_book_name(const nd_bible *b, size_t book);
const char *nd_bible_book_abbr(const nd_bible *b, size_t book);
unsigned nd_bible_book_section(const nd_bible *b, size_t book);
size_t nd_bible_chapter_count(const nd_bible *b, size_t book);

/* Total chapters in the pack, for the search progress bar. */
size_t nd_bible_total_chapters(const nd_bible *b);

/* The largest chapter in the pack, inflated, as the packer measured it. The
 * reader sizes its wrap arena from this rather than from a guess, which is
 * the reason the header carries the number at all. */
size_t nd_bible_max_raw(const nd_bible *b);

/* Case-insensitive, matching the abbreviation first and then a prefix of the
 * display name, so "joh" finds John and "1 co" finds 1 Corinthians. Returns
 * the book index or -1. */
int32_t nd_bible_find_book(const nd_bible *b, const char *needle);

/* Inflates one chapter into the handle's single scratch buffer, replacing
 * whatever was there. chapter is 0-based. Loading a chapter that is already
 * loaded is free. */
nd_err nd_bible_load(nd_bible *b, size_t book, size_t chapter);

size_t nd_bible_verse_count(const nd_bible *b);

/* Verse text, 1-BASED to match how it is printed. "" outside the range.
 * The pointer is into the scratch buffer and is invalidated by the next
 * nd_bible_load(). */
const char *nd_bible_verse(const nd_bible *b, size_t verse);

/* ------------------------------------------------------------------ *
 * Gen Z mode
 * ------------------------------------------------------------------ */

/* Rewrites one verse into the register of somebody who has never read a book.
 *
 * Word-for-word from a fixed table, matched on whole words only and with the
 * source's capitalisation carried over, plus an interjection at the front and
 * a tag at the end chosen by `seed`. Passing the verse's own (book, chapter,
 * verse) as the seed is what makes a verse read the same every time it is
 * drawn -- a random() here would reshuffle the text under a scrolling reader,
 * which looks like a rendering bug rather than a joke.
 *
 * Writes at most out_sz bytes including the NUL and returns the length it
 * wanted to write, snprintf-style, so truncation is (return >= out_sz).
 * Output is pure ASCII. */
size_t nd_genz(char *out, size_t out_sz, const char *in, uint32_t seed);

/* The substitution table, exposed for the unit test. n_entries is written
 * with the count; the returned array is static and outlives everything. */
typedef struct {
    const char *from;
    const char *to;
} nd_genz_pair;

const nd_genz_pair *nd_genz_table(size_t *n_entries);

/* The seed a verse gets. Exposed so the test can prove it is stable. */
uint32_t nd_genz_seed(size_t book, size_t chapter, size_t verse);

#ifdef __cplusplus
}
#endif

#endif /* ND_BIBLE_H_INCLUDED */
