/* nd_id3.c -- a bounded reader for the four ID3v2 frames the Music app draws.
 *
 * See nd_id3.h for the interface and for why this is not a library binding.
 * This file is the whole implementation: header, extended header,
 * unsynchronisation, ID3v2.2/2.3/2.4 frame walks, four text encodings and the
 * two picture frame layouts. About four hundred lines against mutagen's
 * fourteen thousand, because it reads four frames and writes none.
 *
 * ============ THE ONE THING TO KEEP IN MIND WHILE EDITING ============
 *
 * EVERY LENGTH IN THIS FILE CAME OUT OF THE FILE. There is no length here
 * that the program chose. So the pattern below is used without exception:
 *
 *     if (claimed > remaining)
 *         break;
 *
 * and never `pos + claimed > len`, which overflows for a claimed size near
 * SIZE_MAX and then compares true when it should compare false. A tag with
 * one frame claiming 0xFFFFFFFF bytes is not hypothetical -- it is what a
 * truncated download looks like.
 *
 * ============ WHERE THE BEHAVIOUR CAME FROM ============
 *
 * mutagen, because that is what the Python calls and what the phone's
 * existing files were written to be read by. Three of its choices are
 * reproduced deliberately and are called out at the point they are made:
 * a v2.2 tag with the compression flag set is abandoned whole, a UTF-16
 * value with no BOM is read little-endian, and the tag-level
 * unsynchronisation flag is applied before the extended header is skipped
 * for v2.2/v2.3 but per-frame for v2.4.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_id3.h"
#include "nd_log.h"
#include "nd_paths.h"

/* ------------------------------------------------------------------ *
 * Integers off the wire -- byte by byte, no casts over a pointer
 * ------------------------------------------------------------------ */

static uint32_t be24(const uint8_t *p)
{
    return ((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | (uint32_t)p[2];
}

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* A "syncsafe" integer: seven bits per byte, so no byte can be 0xFF and the
 * size field can never look like an MPEG sync word. mutagen's BitPaddedInt
 * masks the eighth bit rather than rejecting it, and so does this -- a
 * tagger that wrote a plain integer gets a wrong (smaller) size and the walk
 * stops early, which is a bad tag read badly and not a crash. */
static uint32_t syncsafe32(const uint8_t *p)
{
    return ((uint32_t)(p[0] & 0x7Fu) << 21) | ((uint32_t)(p[1] & 0x7Fu) << 14) |
           ((uint32_t)(p[2] & 0x7Fu) << 7) | (uint32_t)(p[3] & 0x7Fu);
}

/* ------------------------------------------------------------------ *
 * Unsynchronisation
 * ------------------------------------------------------------------ */

/* "FF 00" -> "FF", in place. The 0x00 exists so that no two bytes in the tag
 * can be mistaken for an MPEG frame sync by a decoder that does not
 * understand ID3; removing it is the reader's job. Returns the new length,
 * which is never larger than the old one, so this can never overrun. */
static size_t deunsync(uint8_t *p, size_t len)
{
    size_t r = 0u;
    size_t w = 0u;

    while (r < len) {
        p[w] = p[r];
        w++;
        if (p[r] == 0xFFu && (r + 1u) < len && p[r + 1u] == 0x00u)
            r += 2u;
        else
            r += 1u;
    }
    return w;
}

/* ------------------------------------------------------------------ *
 * UTF-8 output
 * ------------------------------------------------------------------ */

/* Append one codepoint. False when it would not fit WITH its terminator, and
 * the buffer is left exactly as it was -- a half-written sequence is worse
 * than a short string. */
static bool append_cp(char *out, size_t out_sz, size_t *n, uint32_t cp)
{
    uint8_t seq[4];
    size_t k;

    if (cp < 0x80u) {
        seq[0] = (uint8_t)cp;
        k = 1u;
    } else if (cp < 0x800u) {
        seq[0] = (uint8_t)(0xC0u | (cp >> 6));
        seq[1] = (uint8_t)(0x80u | (cp & 0x3Fu));
        k = 2u;
    } else if (cp < 0x10000u) {
        seq[0] = (uint8_t)(0xE0u | (cp >> 12));
        seq[1] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        seq[2] = (uint8_t)(0x80u | (cp & 0x3Fu));
        k = 3u;
    } else {
        seq[0] = (uint8_t)(0xF0u | (cp >> 18));
        seq[1] = (uint8_t)(0x80u | ((cp >> 12) & 0x3Fu));
        seq[2] = (uint8_t)(0x80u | ((cp >> 6) & 0x3Fu));
        seq[3] = (uint8_t)(0x80u | (cp & 0x3Fu));
        k = 4u;
    }
    if (*n + k + 1u > out_sz)
        return false;
    memcpy(out + *n, seq, k);
    *n += k;
    return true;
}

/* How many bytes the sequence starting with b claims. 0 for a byte that
 * cannot start one, which includes every continuation byte. */
static size_t utf8_seq_len(uint8_t b)
{
    if (b < 0x80u)
        return 1u;
    if ((b & 0xE0u) == 0xC0u)
        return 2u;
    if ((b & 0xF0u) == 0xE0u)
        return 3u;
    if ((b & 0xF8u) == 0xF0u)
        return 4u;
    return 0u;
}

size_t nd_id3_text_utf8(char *out, size_t out_sz, uint8_t encoding, const uint8_t *in, size_t len)
{
    size_t n = 0u;
    size_t i = 0u;

    if (out == NULL || out_sz == 0u)
        return 0u;
    out[0] = '\0';
    if (in == NULL)
        return 0u;

    if (encoding == 0u) {
        /* ISO-8859-1: the byte IS the codepoint. 0xE9 is not a UTF-8 lead
         * byte, it is U+00E9, and widening it here is the whole conversion. */
        for (i = 0u; i < len; i++) {
            if (in[i] == 0u)
                break;
            if (!append_cp(out, out_sz, &n, (uint32_t)in[i]))
                break;
        }
    } else if (encoding == 1u || encoding == 2u) {
        bool big = (encoding == 2u);

        if (encoding == 1u) {
            if (len >= 2u && in[0] == 0xFFu && in[1] == 0xFEu) {
                big = false;
                i = 2u;
            } else if (len >= 2u && in[0] == 0xFEu && in[1] == 0xFFu) {
                big = true;
                i = 2u;
            } else {
                /* No BOM where the spec demands one. mutagen's UTF-16 codec
                 * falls back to little-endian rather than raising, and files
                 * in the wild rely on it. */
                big = false;
            }
        }
        while ((i + 1u) < len) {
            uint32_t u = big ? (((uint32_t)in[i] << 8) | (uint32_t)in[i + 1u])
                             : (((uint32_t)in[i + 1u] << 8) | (uint32_t)in[i]);

            i += 2u;
            if (u == 0u)
                break;
            if (u >= 0xD800u && u <= 0xDBFFu && (i + 1u) < len) {
                uint32_t lo = big ? (((uint32_t)in[i] << 8) | (uint32_t)in[i + 1u])
                                  : (((uint32_t)in[i + 1u] << 8) | (uint32_t)in[i]);

                if (lo >= 0xDC00u && lo <= 0xDFFFu) {
                    u = 0x10000u + ((u - 0xD800u) << 10) + (lo - 0xDC00u);
                    i += 2u;
                }
            }
            if (!append_cp(out, out_sz, &n, u))
                break;
        }
    } else if (encoding == 3u) {
        /* Already UTF-8. Copied a whole sequence at a time so a truncation
         * cannot cut one in half -- half a sequence renders as a replacement
         * glyph on the panel and as a decode error in anything downstream. */
        while (i < len) {
            size_t k;

            if (in[i] == 0u)
                break;
            k = utf8_seq_len(in[i]);
            if (k == 0u || (len - i) < k)
                break;
            if (n + k + 1u > out_sz)
                break;
            memcpy(out + n, in + i, k);
            n += k;
            i += k;
        }
    } else {
        return 0u; /* an encoding byte no version of ID3 defines */
    }

    out[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------ *
 * Frames
 * ------------------------------------------------------------------ */

/* A frame identifier is upper-case letters and digits. The first zero byte
 * where an identifier should be is the start of the tag's padding, which is
 * the ordinary way a walk ends; anything else there means the stream is no
 * longer where we think it is, and continuing would be reading noise. */
static bool valid_frame_id(const uint8_t *id, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (!((id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= '0' && id[i] <= '9')))
            return false;
    }
    return true;
}

static void take_text(char *dst, size_t dst_sz, bool *flag, const uint8_t *body, size_t blen)
{
    if (blen == 0u) {
        /* A text frame with no encoding byte at all. mutagen gives an empty
         * string rather than dropping the frame, and the app then draws an
         * empty line instead of the filename -- so the flag is set. */
        dst[0] = '\0';
        *flag = true;
        return;
    }
    (void)nd_id3_text_utf8(dst, dst_sz, body[0], body + 1, blen - 1u);
    *flag = true;
}

/* APIC (2.3/2.4) and PIC (2.2) differ only in what sits between the encoding
 * byte and the description: a NUL-terminated MIME string, or three fixed
 * bytes of image format. `fixed_fmt` picks which.
 *
 * Returns false when the frame is malformed, which is not an error -- it is
 * a picture that is not offered to the callback. */
static bool picture_payload(const uint8_t *body, size_t blen, bool fixed_fmt, const uint8_t **data,
                            size_t *data_len)
{
    uint8_t enc;
    size_t i;

    if (blen < 2u)
        return false;
    enc = body[0];
    i = 1u;

    if (fixed_fmt) {
        if ((blen - i) < 3u)
            return false;
        i += 3u; /* "JPG" / "PNG", not terminated */
    } else {
        while (i < blen && body[i] != 0u)
            i++;
        if (i >= blen)
            return false;
        i++; /* the MIME string's terminator */
    }

    if (i >= blen)
        return false;
    i++; /* the picture-type byte */

    /* The description is terminated the way its ENCODING is terminated: one
     * NUL for the byte encodings, two for the UTF-16 pair -- and the pair is
     * aligned to the start of the description, so it is stepped two at a
     * time and a lone 0x00 in the low half of a character is not a
     * terminator. */
    if (enc == 1u || enc == 2u) {
        while ((i + 1u) < blen && !(body[i] == 0u && body[i + 1u] == 0u))
            i += 2u;
        if ((i + 1u) >= blen)
            return false;
        i += 2u;
    } else {
        while (i < blen && body[i] != 0u)
            i++;
        if (i >= blen)
            return false;
        i++;
    }

    if (i >= blen)
        return false;
    *data = body + i;
    *data_len = blen - i;
    return true;
}

/* ID3v2.2: three-character identifiers, a plain 24-bit size, no frame flags. */
static void walk_v22(uint8_t *p, size_t len, nd_id3 *out, nd_id3_pic_fn on_pic, void *ctx)
{
    size_t pos = 0u;

    while ((len - pos) >= 6u) {
        const uint8_t *id = p + pos;
        uint32_t size;
        uint8_t *body;
        size_t blen;

        if (id[0] == 0u)
            break; /* padding */
        if (!valid_frame_id(id, 3u))
            break;
        size = be24(p + pos + 3u);
        pos += 6u;
        if ((size_t)size > (len - pos))
            break;
        body = p + pos;
        blen = (size_t)size;
        pos += blen;

        if (memcmp(id, "TT2", 3) == 0)
            take_text(out->title, sizeof out->title, &out->has_title, body, blen);
        else if (memcmp(id, "TP1", 3) == 0)
            take_text(out->artist, sizeof out->artist, &out->has_artist, body, blen);
        else if (memcmp(id, "TAL", 3) == 0)
            take_text(out->album, sizeof out->album, &out->has_album, body, blen);
        else if (on_pic != NULL && memcmp(id, "PIC", 3) == 0) {
            const uint8_t *data;
            size_t data_len;

            if (picture_payload(body, blen, true, &data, &data_len) &&
                on_pic(data, data_len, ctx))
                return;
        }
    }
}

/* ID3v2.3 and 2.4: four-character identifiers, a ten-byte frame header, two
 * flag bytes whose meanings differ between the two versions. */
static void walk_v23_v24(uint8_t *p, size_t len, uint8_t ver, bool tag_unsync, nd_id3 *out,
                         nd_id3_pic_fn on_pic, void *ctx)
{
    size_t pos = 0u;

    while ((len - pos) >= 10u) {
        const uint8_t *id = p + pos;
        uint32_t size;
        uint8_t fmt_flags;
        uint8_t *body;
        size_t blen;
        bool skip = false;

        if (id[0] == 0u)
            break;
        if (!valid_frame_id(id, 4u))
            break;

        size = (ver >= 4u) ? syncsafe32(p + pos + 4u) : be32(p + pos + 4u);
        fmt_flags = p[pos + 9u];
        pos += 10u;
        if ((size_t)size > (len - pos))
            break;
        body = p + pos;
        blen = (size_t)size;
        pos += blen;

        if (ver >= 4u) {
            /* Compression (0x08) and encryption (0x04) are recognised and the
             * frame is dropped. See the header: inflating an attacker's zlib
             * stream to learn an album name is not a trade worth making. */
            if ((fmt_flags & 0x0Cu) != 0u)
                skip = true;
            if (!skip && (fmt_flags & 0x40u) != 0u) { /* group identifier */
                if (blen < 1u)
                    skip = true;
                else {
                    body++;
                    blen--;
                }
            }
            if (!skip && (fmt_flags & 0x01u) != 0u) { /* data length indicator */
                if (blen < 4u)
                    skip = true;
                else {
                    body += 4u;
                    blen -= 4u;
                }
            }
            if (!skip && ((fmt_flags & 0x02u) != 0u || tag_unsync))
                blen = deunsync(body, blen);
        } else {
            if ((fmt_flags & 0xC0u) != 0u) /* compression / encryption */
                skip = true;
            if (!skip && (fmt_flags & 0x20u) != 0u) { /* grouping */
                if (blen < 1u)
                    skip = true;
                else {
                    body++;
                    blen--;
                }
            }
        }
        if (skip)
            continue;

        if (memcmp(id, "TIT2", 4) == 0)
            take_text(out->title, sizeof out->title, &out->has_title, body, blen);
        else if (memcmp(id, "TPE1", 4) == 0)
            take_text(out->artist, sizeof out->artist, &out->has_artist, body, blen);
        else if (memcmp(id, "TALB", 4) == 0)
            take_text(out->album, sizeof out->album, &out->has_album, body, blen);
        else if (on_pic != NULL && memcmp(id, "APIC", 4) == 0) {
            const uint8_t *data;
            size_t data_len;

            if (picture_payload(body, blen, false, &data, &data_len) &&
                on_pic(data, data_len, ctx))
                return;
        }
    }
}

/* ------------------------------------------------------------------ *
 * The tag
 * ------------------------------------------------------------------ */

/* `buf` holds the tag body -- everything after the ten-byte header -- and is
 * MODIFIED in place by the unsynchronisation pass. */
static nd_err parse_body(uint8_t *buf, size_t len, uint8_t ver, uint8_t flags, nd_id3 *out,
                         nd_id3_pic_fn on_pic, void *ctx)
{
    bool unsync = (flags & 0x80u) != 0u;
    size_t pos = 0u;

    if (ver == 2u) {
        /* In 2.2 the 0x40 bit means "the whole tag is compressed", and the
         * scheme was never specified. mutagen refuses the tag rather than
         * guessing, and so does this. */
        if ((flags & 0x40u) != 0u)
            return ND_ERR_UNSUPPORTED;
    }

    /* For 2.2 and 2.3 the flag applies to everything after the header,
     * INCLUDING the extended header, so it is undone before that is skipped.
     * 2.4 moved it onto the individual frames and it is handled there. */
    if (unsync && ver < 4u)
        len = deunsync(buf, len);

    if ((flags & 0x40u) != 0u && ver >= 3u) {
        uint32_t ext;

        if (len < 4u)
            return ND_ERR_PARSE;
        if (ver >= 4u) {
            /* 2.4: a syncsafe size that INCLUDES its own four bytes. */
            ext = syncsafe32(buf);
            if (ext < 6u || (size_t)ext > len)
                return ND_ERR_PARSE;
            pos = (size_t)ext;
        } else {
            /* 2.3: a plain size of what FOLLOWS the size field. */
            ext = be32(buf);
            if ((size_t)ext > (len - 4u))
                return ND_ERR_PARSE;
            pos = 4u + (size_t)ext;
        }
    }

    out->version = ver;
    if (ver == 2u)
        walk_v22(buf + pos, len - pos, out, on_pic, ctx);
    else
        walk_v23_v24(buf + pos, len - pos, ver, unsync, out, on_pic, ctx);
    return ND_OK;
}

/* The ten-byte header, shared by the file and the memory entry points. */
static nd_err read_header(const uint8_t *hdr, uint8_t *ver_out, uint8_t *flags_out,
                          uint32_t *size_out)
{
    uint8_t ver;

    if (hdr[0] != 'I' || hdr[1] != 'D' || hdr[2] != '3')
        return ND_ERR_NOTFOUND;
    ver = hdr[3];
    if (ver < 2u || ver > 4u)
        return ND_ERR_NOTFOUND; /* 2.5 does not exist; 0xFF is a version marker */
    *ver_out = ver;
    *flags_out = hdr[5];
    *size_out = syncsafe32(hdr + 6);
    if (*size_out == 0u)
        return ND_ERR_PARSE;
    if (*size_out > ND_ID3_TAG_MAX)
        return ND_ERR_PARSE;
    return ND_OK;
}

nd_err nd_id3_parse(const uint8_t *buf, size_t len, nd_id3 *out, nd_id3_pic_fn on_pic,
                    void *pic_ctx)
{
    uint8_t ver = 0u;
    uint8_t flags = 0u;
    uint32_t size = 0u;
    uint8_t *copy;
    nd_err rc;

    if (out == NULL)
        return ND_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (buf == NULL || len < ND_ID3_HEADER_LEN)
        return ND_ERR_NOTFOUND;

    rc = read_header(buf, &ver, &flags, &size);
    if (rc != ND_OK)
        return rc;

    /* A truncated file is read as far as it goes, which is what mutagen does
     * with a short tag: the frames that are there still parse. */
    if ((size_t)size > (len - ND_ID3_HEADER_LEN))
        size = (uint32_t)(len - ND_ID3_HEADER_LEN);
    if (size == 0u)
        return ND_ERR_PARSE;

    /* owned here; freed before every return below. The unsynchronisation
     * pass rewrites bytes, and the caller's buffer is const. */
    copy = malloc((size_t)size);
    if (copy == NULL)
        return ND_ERR_NOMEM;
    memcpy(copy, buf + ND_ID3_HEADER_LEN, (size_t)size);

    rc = parse_body(copy, (size_t)size, ver, flags, out, on_pic, pic_ctx);
    free(copy);
    return rc;
}

nd_err nd_id3_read(const char *path, nd_id3 *out, nd_id3_pic_fn on_pic, void *pic_ctx)
{
    char resolved[ND_PATH_MAX];
    uint8_t hdr[ND_ID3_HEADER_LEN];
    uint8_t *buf = NULL;
    FILE *f = NULL;
    uint8_t ver = 0u;
    uint8_t flags = 0u;
    uint32_t size = 0u;
    size_t got;
    nd_err rc;

    if (out == NULL)
        return ND_ERR_INVAL;
    memset(out, 0, sizeof *out);
    if (path == NULL || path[0] == '\0')
        return ND_ERR_INVAL;
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK)
        return ND_ERR_TOOLONG;

    f = fopen(resolved, "rb");
    if (f == NULL) {
        rc = ND_ERR_IO;
        goto done;
    }
    if (fread(hdr, 1u, sizeof hdr, f) != sizeof hdr) {
        rc = ND_ERR_NOTFOUND; /* shorter than a header: no tag, not an error */
        goto done;
    }
    rc = read_header(hdr, &ver, &flags, &size);
    if (rc != ND_OK)
        goto done;

    /* size is <= ND_ID3_TAG_MAX by read_header, so this allocation is
     * bounded at 1 MB however large the file claims its tag is. */
    buf = malloc((size_t)size);
    if (buf == NULL) {
        rc = ND_ERR_NOMEM;
        goto done;
    }
    got = fread(buf, 1u, (size_t)size, f);
    if (got == 0u) {
        rc = ND_ERR_PARSE;
        goto done;
    }
    rc = parse_body(buf, got, ver, flags, out, on_pic, pic_ctx);

done:
    free(buf);
    if (f != NULL)
        (void)fclose(f);
    return rc;
}
