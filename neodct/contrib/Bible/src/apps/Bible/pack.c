/* pack.c -- the .ndb reader. Format and rationale live in bible.h.
 *
 * ============ THE MEMORY BUDGET, WHICH IS THE WHOLE DESIGN ============
 *
 * An app process on this phone has a few megabytes to play with and the pack
 * is 1.7 MB on disk holding 4.8 MB of text. Nothing here ever holds more than
 * one chapter.
 *
 * Resident, for the life of the handle:
 *
 *     index      18 KB   book table + chapter table + name pool, read once
 *     dictionary 32 KB   the preset zlib window, read once
 *     raw        18 KB   max_raw + 1, the inflate target, reused
 *     verses      1 KB   offsets into raw, sized by the pack's own worst case
 *     ----------------
 *                69 KB
 *
 * Per chapter opened: one fseek, one fread of a couple of kilobytes, one
 * inflate. No allocation at all -- every buffer above is sized at open() from
 * a number the packer stamped in the header, which is the reason the header
 * carries max_raw rather than letting the reader discover it.
 *
 * ============ WHY THE INDEX IS VALIDATED IN FULL AT OPEN ============
 *
 * 1,402 chapter entries are checked against the blob length in a loop that
 * runs in microseconds. The alternative is discovering a truncated pack in the
 * middle of a scroll, four hundred verses in, as a short read that has to be
 * turned into something the reader can draw. Failing in nd_bible_open() lets
 * every call after it assume its own index is sane.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zlib.h>

#include "nd_log.h"
#include "nd_paths.h"
#include "nd_types.h"

#include "bible.h"

#define BIBLE_TAG "bible"

/* Header field offsets. Spelled out rather than read through a packed struct:
 * the file is little-endian by definition and the host might not be, and a
 * __attribute__((packed)) struct would hide that. */
#define HDR_VERSION    8u
#define HDR_FLAGS      10u
#define HDR_N_BOOKS    12u
#define HDR_N_CHAPTERS 16u
#define HDR_BOOK_OFF   20u
#define HDR_CHAP_OFF   24u
#define HDR_POOL_OFF   28u
#define HDR_POOL_LEN   32u
#define HDR_BLOB_OFF   36u
#define HDR_BLOB_LEN   40u
#define HDR_MAX_RAW    44u
#define HDR_DICT_OFF   48u
#define HDR_DICT_LEN   52u
#define HDR_NAME       56u

/* Book entry field offsets, within a 16-byte entry. */
#define BK_ABBR    0u
#define BK_NAME    4u
#define BK_N_CHAP  6u
#define BK_FIRST   8u
#define BK_SECTION 12u

/* Chapter entry field offsets, within a 12-byte entry. */
#define CH_OFF      0u
#define CH_COMP_LEN 4u
#define CH_RAW_LEN  8u
#define CH_N_VERSES 10u

struct nd_bible {
    FILE *fp;
    unsigned flags;

    /* The index: book table, chapter table and name pool, contiguous, exactly
     * as they sit in the file, so the tables can be addressed by the file's
     * own offsets minus book_off. */
    uint8_t *index;
    size_t index_len;
    uint32_t book_base; /* the file offset the index buffer starts at */
    uint32_t book_off;
    uint32_t chap_off;
    uint32_t pool_off;
    uint32_t pool_len;

    size_t n_books;
    size_t n_chapters;

    uint32_t blob_off;
    uint32_t blob_len;
    uint32_t max_raw;

    uint8_t *zdict;
    uint32_t zdict_len;

    char translation[ND_NDB_NAME_MAX];

    /* The one loaded chapter. `raw` holds its verses with the separating
     * newlines overwritten by NULs, so a verse is a plain C string pointing
     * into it. */
    char *raw;
    uint32_t *verse_off;
    size_t verse_cap;
    size_t n_verses;

    size_t cur_book;
    size_t cur_chapter;
    bool loaded;
};

/* ------------------------------------------------------------------ *
 * Little-endian scalar reads
 * ------------------------------------------------------------------ */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ *
 * Table addressing
 * ------------------------------------------------------------------ */

static const uint8_t *book_at(const nd_bible *b, size_t i)
{
    return b->index + (b->book_off - b->book_base) + i * ND_NDB_BOOK_SZ;
}

static const uint8_t *chap_at(const nd_bible *b, size_t i)
{
    return b->index + (b->chap_off - b->book_base) + i * ND_NDB_CHAP_SZ;
}

static const char *pool_at(const nd_bible *b, uint32_t off)
{
    if (off >= b->pool_len)
        return "";
    return (const char *)(b->index + (b->pool_off - b->book_base) + off);
}

/* ------------------------------------------------------------------ *
 * open / close
 * ------------------------------------------------------------------ */

static nd_err read_at(FILE *fp, long off, void *dst, size_t len)
{
    if (fseek(fp, off, SEEK_SET) != 0)
        return ND_ERR_IO;
    if (len != 0u && fread(dst, 1u, len, fp) != len)
        return ND_ERR_IO;
    return ND_OK;
}

/* Every consistency check the rest of the file then gets to assume. Returns
 * the widest chapter in verses, which is what sizes the verse-offset array. */
static nd_err validate(nd_bible *b, long file_len, size_t *max_verses_out)
{
    size_t i;
    size_t max_verses = 0u;

    if (b->n_books == 0u || b->n_chapters == 0u)
        return ND_ERR_PARSE;
    if (b->max_raw == 0u || b->max_raw > ND_NDB_RAW_MAX)
        return ND_ERR_PARSE;
    if (b->zdict_len > ND_NDB_DICT_MAX)
        return ND_ERR_PARSE;
    if ((uint64_t)b->blob_off + b->blob_len > (uint64_t)file_len)
        return ND_ERR_PARSE;

    for (i = 0u; i < b->n_books; i++) {
        const uint8_t *e = book_at(b, i);
        uint32_t first = rd32(e + BK_FIRST);
        uint16_t n_ch = rd16(e + BK_N_CHAP);

        if (rd16(e + BK_NAME) >= b->pool_len)
            return ND_ERR_PARSE;
        if ((uint64_t)first + n_ch > (uint64_t)b->n_chapters)
            return ND_ERR_PARSE;
    }

    for (i = 0u; i < b->n_chapters; i++) {
        const uint8_t *e = chap_at(b, i);
        uint32_t off = rd32(e + CH_OFF);
        uint32_t clen = rd32(e + CH_COMP_LEN);
        uint16_t rlen = rd16(e + CH_RAW_LEN);
        uint16_t nv = rd16(e + CH_N_VERSES);

        if ((uint64_t)off + clen > (uint64_t)b->blob_len)
            return ND_ERR_PARSE;
        if (rlen > b->max_raw)
            return ND_ERR_PARSE;
        /* n verses are n-1 newlines plus the text, so a chapter cannot claim
         * more verses than its inflated length can hold separators for. */
        if (nv > 0u && (uint32_t)(nv - 1u) > rlen)
            return ND_ERR_PARSE;
        if ((size_t)nv > max_verses)
            max_verses = (size_t)nv;
    }

    /* The pool must be NUL-terminated or pool_at() would run off the end. */
    if (b->pool_len == 0u ||
        b->index[(b->pool_off - b->book_base) + b->pool_len - 1u] != 0u)
        return ND_ERR_PARSE;

    *max_verses_out = max_verses;
    return ND_OK;
}

nd_err nd_bible_open(nd_bible **out, const char *path)
{
    char resolved[ND_PATH_MAX];
    uint8_t hdr[ND_NDB_HDR_SZ];
    nd_bible *b;
    nd_err rc;
    long file_len;
    size_t max_verses = 0u;
    uint32_t index_end;

    if (out == NULL || path == NULL)
        return ND_ERR_INVAL;
    *out = NULL;

    rc = nd_path_resolve(resolved, sizeof resolved, path);
    if (rc != ND_OK)
        return rc;

    b = calloc(1u, sizeof *b);
    if (b == NULL)
        return ND_ERR_NOMEM;
    b->cur_book = (size_t)-1;
    b->cur_chapter = (size_t)-1;

    b->fp = fopen(resolved, "rb");
    if (b->fp == NULL) {
        free(b);
        return ND_ERR_NOTFOUND;
    }
    if (fseek(b->fp, 0, SEEK_END) != 0) {
        rc = ND_ERR_IO;
        goto fail;
    }
    file_len = ftell(b->fp);
    if (file_len < (long)ND_NDB_HDR_SZ) {
        rc = ND_ERR_PARSE;
        goto fail;
    }

    rc = read_at(b->fp, 0, hdr, sizeof hdr);
    if (rc != ND_OK)
        goto fail;
    if (memcmp(hdr, ND_NDB_MAGIC, ND_NDB_MAGIC_N) != 0) {
        rc = ND_ERR_PARSE;
        goto fail;
    }
    if (rd16(hdr + HDR_VERSION) != ND_NDB_VERSION) {
        rc = ND_ERR_UNSUPPORTED;
        goto fail;
    }

    b->flags = rd16(hdr + HDR_FLAGS);
    b->n_books = rd16(hdr + HDR_N_BOOKS);
    b->n_chapters = rd32(hdr + HDR_N_CHAPTERS);
    b->book_off = rd32(hdr + HDR_BOOK_OFF);
    b->chap_off = rd32(hdr + HDR_CHAP_OFF);
    b->pool_off = rd32(hdr + HDR_POOL_OFF);
    b->pool_len = rd32(hdr + HDR_POOL_LEN);
    b->blob_off = rd32(hdr + HDR_BLOB_OFF);
    b->blob_len = rd32(hdr + HDR_BLOB_LEN);
    b->max_raw = rd32(hdr + HDR_MAX_RAW);
    b->zdict_len = ((b->flags & ND_NDB_F_DICT) != 0u) ? rd32(hdr + HDR_DICT_LEN) : 0u;
    memcpy(b->translation, hdr + HDR_NAME, ND_NDB_NAME_MAX);
    b->translation[ND_NDB_NAME_MAX - 1u] = '\0';

    /* Only deflated packs exist; the flag is here so a future stored-plain
     * variant is a flag rather than a version break. */
    if ((b->flags & ND_NDB_F_DEFLATE) == 0u) {
        rc = ND_ERR_UNSUPPORTED;
        goto fail;
    }

    /* The three tables are contiguous and in this order, which the packer
     * guarantees; reading them as one block is one seek instead of three. */
    if (b->book_off != ND_NDB_HDR_SZ || b->chap_off < b->book_off ||
        b->pool_off < b->chap_off) {
        rc = ND_ERR_PARSE;
        goto fail;
    }
    index_end = b->pool_off + b->pool_len;
    if ((uint64_t)index_end > (uint64_t)file_len) {
        rc = ND_ERR_PARSE;
        goto fail;
    }
    b->book_base = b->book_off;
    b->index_len = (size_t)(index_end - b->book_base);
    b->index = malloc(b->index_len);
    if (b->index == NULL) {
        rc = ND_ERR_NOMEM;
        goto fail;
    }
    rc = read_at(b->fp, (long)b->book_base, b->index, b->index_len);
    if (rc != ND_OK)
        goto fail;

    /* Sized from the tables, so this must follow the read and precede any
     * use of book_at()/chap_at() outside validate() itself. */
    if ((uint64_t)b->book_off + (uint64_t)b->n_books * ND_NDB_BOOK_SZ > (uint64_t)b->chap_off ||
        (uint64_t)b->chap_off + (uint64_t)b->n_chapters * ND_NDB_CHAP_SZ > (uint64_t)b->pool_off) {
        rc = ND_ERR_PARSE;
        goto fail;
    }
    rc = validate(b, file_len, &max_verses);
    if (rc != ND_OK)
        goto fail;

    if (b->zdict_len != 0u) {
        b->zdict = malloc(b->zdict_len);
        if (b->zdict == NULL) {
            rc = ND_ERR_NOMEM;
            goto fail;
        }
        rc = read_at(b->fp, (long)rd32(hdr + HDR_DICT_OFF), b->zdict, b->zdict_len);
        if (rc != ND_OK)
            goto fail;
    }

    b->raw = malloc((size_t)b->max_raw + 1u);
    b->verse_cap = (max_verses != 0u) ? max_verses : 1u;
    b->verse_off = malloc(b->verse_cap * sizeof *b->verse_off);
    if (b->raw == NULL || b->verse_off == NULL) {
        rc = ND_ERR_NOMEM;
        goto fail;
    }

    *out = b;
    return ND_OK;

fail:
    nd_bible_close(b);
    return rc;
}

void nd_bible_close(nd_bible *b)
{
    if (b == NULL)
        return;
    if (b->fp != NULL)
        (void)fclose(b->fp);
    free(b->index);
    free(b->zdict);
    free(b->raw);
    free(b->verse_off);
    free(b);
}

/* ------------------------------------------------------------------ *
 * The index, read-only
 * ------------------------------------------------------------------ */

const char *nd_bible_translation(const nd_bible *b)
{
    return (b != NULL) ? b->translation : "";
}

size_t nd_bible_book_count(const nd_bible *b)
{
    return (b != NULL) ? b->n_books : 0u;
}

size_t nd_bible_total_chapters(const nd_bible *b)
{
    return (b != NULL) ? b->n_chapters : 0u;
}

size_t nd_bible_max_raw(const nd_bible *b)
{
    return (b != NULL) ? (size_t)b->max_raw : 0u;
}

const char *nd_bible_book_name(const nd_bible *b, size_t book)
{
    if (b == NULL || book >= b->n_books)
        return "";
    return pool_at(b, rd16(book_at(b, book) + BK_NAME));
}

const char *nd_bible_book_abbr(const nd_bible *b, size_t book)
{
    if (b == NULL || book >= b->n_books)
        return "";
    /* The field is four bytes and the packer NUL-pads it, so a three-letter
     * code is already a C string sitting in the mapping. */
    return (const char *)(book_at(b, book) + BK_ABBR);
}

unsigned nd_bible_book_section(const nd_bible *b, size_t book)
{
    if (b == NULL || book >= b->n_books)
        return ND_NDB_OT;
    return book_at(b, book)[BK_SECTION];
}

size_t nd_bible_chapter_count(const nd_bible *b, size_t book)
{
    if (b == NULL || book >= b->n_books)
        return 0u;
    return rd16(book_at(b, book) + BK_N_CHAP);
}

static char lower_ascii(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static int cmp_ci(const char *a, const char *b, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        char ca = lower_ascii(a[i]);
        char cb = lower_ascii(b[i]);

        if (ca != cb || ca == '\0')
            return (int)((unsigned char)ca) - (int)((unsigned char)cb);
    }
    return 0;
}

int32_t nd_bible_find_book(const nd_bible *b, const char *needle)
{
    size_t i;
    size_t len;

    if (b == NULL || needle == NULL || needle[0] == '\0')
        return -1;
    len = strlen(needle);

    /* Abbreviations first and exactly. "JOB" must not be answered by Joel
     * merely because the name search runs earlier and matches no better. */
    for (i = 0u; i < b->n_books; i++) {
        const char *abbr = nd_bible_book_abbr(b, i);

        if (strlen(abbr) == len && cmp_ci(abbr, needle, len) == 0)
            return (int32_t)i;
    }
    /* Then a prefix of the display name, so "1 co" finds 1 Corinthians and
     * "gen" finds Genesis. First match in canonical order wins, which is why
     * "jo" is John rather than Jonah in the New Testament sense people mean. */
    for (i = 0u; i < b->n_books; i++) {
        const char *name = nd_bible_book_name(b, i);

        if (strlen(name) >= len && cmp_ci(name, needle, len) == 0)
            return (int32_t)i;
    }
    return -1;
}

/* ------------------------------------------------------------------ *
 * Loading a chapter
 * ------------------------------------------------------------------ */

/* One chapter's compressed bytes, straight from the file into `dst`. Kept
 * separate so the inflate below reads as the algorithm and not as I/O. */
static nd_err read_chapter_bytes(nd_bible *b, uint32_t off, uint32_t clen, uint8_t **dst)
{
    uint8_t *buf = malloc((clen != 0u) ? clen : 1u);
    nd_err rc;

    if (buf == NULL)
        return ND_ERR_NOMEM;
    rc = read_at(b->fp, (long)(b->blob_off + off), buf, clen);
    if (rc != ND_OK) {
        free(buf);
        return rc;
    }
    *dst = buf;
    return ND_OK;
}

static nd_err inflate_chapter(nd_bible *b, const uint8_t *src, uint32_t clen, uint16_t rlen)
{
    z_stream z;
    int zrc;
    nd_err rc = ND_OK;

    memset(&z, 0, sizeof z);
    if (inflateInit(&z) != Z_OK)
        return ND_ERR_NOMEM;

    z.next_in = (Bytef *)(uintptr_t)src;
    z.avail_in = clen;
    z.next_out = (Bytef *)b->raw;
    z.avail_out = rlen;

    /* With a preset dictionary the first inflate() consumes the header and
     * stops at Z_NEED_DICT; the dictionary goes in there and only there.
     * Without one, the same call runs straight through to Z_STREAM_END. */
    for (;;) {
        zrc = inflate(&z, Z_FINISH);
        if (zrc == Z_NEED_DICT) {
            if (b->zdict == NULL) {
                rc = ND_ERR_PARSE;
                break;
            }
            if (inflateSetDictionary(&z, b->zdict, b->zdict_len) != Z_OK) {
                rc = ND_ERR_PARSE;
                break;
            }
            continue;
        }
        if (zrc == Z_STREAM_END)
            break;
        if (zrc == Z_BUF_ERROR && z.avail_out == 0u) {
            /* Output exactly filled and the stream is not done: the chapter
             * is longer than the index claimed, so the index is wrong. */
            rc = ND_ERR_PARSE;
            break;
        }
        rc = ND_ERR_PARSE;
        break;
    }
    if (rc == ND_OK && z.total_out != (uLong)rlen)
        rc = ND_ERR_PARSE;
    (void)inflateEnd(&z);
    return rc;
}

/* Split the inflated chapter in place: newlines become NULs and each verse's
 * start is recorded. Verse numbering is 1-based and contiguous by the
 * packer's guarantee, so this is the only place that has to know it. */
static void index_verses(nd_bible *b, uint16_t rlen, uint16_t n_verses)
{
    size_t v = 0u;
    uint32_t i;

    b->raw[rlen] = '\0';
    b->n_verses = 0u;
    if (n_verses == 0u)
        return;

    b->verse_off[v++] = 0u;
    for (i = 0u; i < rlen; i++) {
        if (b->raw[i] == '\n') {
            b->raw[i] = '\0';
            if (v < b->verse_cap)
                b->verse_off[v++] = i + 1u;
        }
    }
    b->n_verses = v;
}

nd_err nd_bible_load(nd_bible *b, size_t book, size_t chapter)
{
    const uint8_t *bk;
    const uint8_t *ce;
    uint8_t *comp = NULL;
    uint32_t idx;
    uint16_t rlen;
    uint16_t nv;
    nd_err rc;

    if (b == NULL)
        return ND_ERR_INVAL;
    if (book >= b->n_books)
        return ND_ERR_NOTFOUND;
    bk = book_at(b, book);
    if (chapter >= (size_t)rd16(bk + BK_N_CHAP))
        return ND_ERR_NOTFOUND;
    if (b->loaded && b->cur_book == book && b->cur_chapter == chapter)
        return ND_OK;

    idx = rd32(bk + BK_FIRST) + (uint32_t)chapter;
    ce = chap_at(b, idx);
    rlen = rd16(ce + CH_RAW_LEN);
    nv = rd16(ce + CH_N_VERSES);

    /* A chapter the source had nothing for. Not an error: mkbible.py fills
     * gaps so that chapter numbering stays arithmetic, and the reader shows
     * the empty page rather than refusing to open the book. */
    if (rlen == 0u || nv == 0u) {
        b->raw[0] = '\0';
        b->n_verses = 0u;
        b->cur_book = book;
        b->cur_chapter = chapter;
        b->loaded = true;
        return ND_OK;
    }

    rc = read_chapter_bytes(b, rd32(ce + CH_OFF), rd32(ce + CH_COMP_LEN), &comp);
    if (rc != ND_OK)
        return rc;
    rc = inflate_chapter(b, comp, rd32(ce + CH_COMP_LEN), rlen);
    free(comp);
    if (rc != ND_OK) {
        /* Leave no half-inflated chapter behind for the next verse read. */
        b->loaded = false;
        b->n_verses = 0u;
        nd_log(BIBLE_TAG, "chapter %u of %s inflate failed: %s", (unsigned)chapter + 1u,
               nd_bible_book_abbr(b, book), nd_strerror(rc));
        return rc;
    }

    index_verses(b, rlen, nv);
    b->cur_book = book;
    b->cur_chapter = chapter;
    b->loaded = true;
    return ND_OK;
}

size_t nd_bible_verse_count(const nd_bible *b)
{
    return (b != NULL && b->loaded) ? b->n_verses : 0u;
}

const char *nd_bible_verse(const nd_bible *b, size_t verse)
{
    if (b == NULL || !b->loaded || verse == 0u || verse > b->n_verses)
        return "";
    return b->raw + b->verse_off[verse - 1u];
}
