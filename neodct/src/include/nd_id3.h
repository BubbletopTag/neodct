/* nd_id3.h -- the four ID3v2 frames the Music app puts on screen, read out of
 * a file that a user may have put on the SD card themselves.
 *
 * The Python calls mutagen: `MP3(path, ID3=ID3)`, then `TIT2`, `TPE1`, `TALB`
 * and the first `APIC` frame. mutagen is 14,000 lines and is not in the
 * image, so this is a deliberately small reader for exactly those four
 * frames and nothing else. It does not write tags, it does not read ID3v1,
 * it does not read Vorbis comments, and it reports no error a caller could
 * act on beyond "there was nothing to read".
 *
 * ============ THIS PARSES ATTACKER-INFLUENCED BYTES ============
 *
 * SECURITY.md's rule for the update system applies here for the same reason:
 * the length fields in an ID3 tag come from the file, the file comes off a
 * FAT32 card, and the card comes from whoever handed it to the user. So:
 *
 *   * The tag is capped at ND_ID3_TAG_MAX before a single byte is allocated,
 *     and the cap is checked against the header's own size field, not
 *     against how much was read.
 *   * Every frame's size is checked against the bytes REMAINING in the tag
 *     before it is used to advance, so a frame claiming 2 GB inside a 40 kB
 *     tag ends the walk instead of running off the end.
 *   * Text is decoded into a fixed buffer and truncated at a codepoint
 *     boundary. There is no allocation on the text path at all.
 *   * A picture is never copied into a caller's buffer. It is handed to a
 *     callback as a pointer into the reader's own scratch, which means the
 *     only heap this module ever holds is one copy of the tag.
 *   * Nothing here recurses.
 *
 * ============ WHAT IT DELIBERATELY DOES NOT DO ============
 *
 * ID3v2.4's per-frame compression and encryption flags are RECOGNISED and the
 * frame is then SKIPPED. Decompressing an attacker's zlib stream to find out
 * what an album is called is not a trade this phone should make, and no
 * shipped tagger writes compressed text frames.
 */

#ifndef ND_ID3_H_INCLUDED
#define ND_ID3_H_INCLUDED

#include "nd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One tag. 192 bytes is about sixty characters of Latin text, and the widest
 * string this app can draw is 116 px at 20 px type -- roughly fifteen. The
 * buffer exists to hold the tag, not to bound the display. */
#define ND_ID3_TEXT_MAX 192

/* The whole tag, header excluded. A syncsafe size field can express 256 MB;
 * a real tag with cover art is 40-200 kB. 1 MB is generous and is what is
 * allocated in the worst case. */
#define ND_ID3_TAG_MAX (1024u * 1024u)

/* The smallest tag that can hold a header and one v2.2 frame. Anything
 * shorter is padding or a lie. */
#define ND_ID3_HEADER_LEN 10

typedef struct {
    /* Empty AND has_* false when the frame was absent; empty with has_* true
     * when the frame was present and held an empty string. The Python
     * distinguishes them -- `if "TIT2" in audio.tags` -- and the difference
     * decides whether the filename or an empty line is drawn. */
    char title[ND_ID3_TEXT_MAX];  /* TIT2 / TT2 */
    char artist[ND_ID3_TEXT_MAX]; /* TPE1 / TP1 */
    char album[ND_ID3_TEXT_MAX];  /* TALB / TAL */
    bool has_title;
    bool has_artist;
    bool has_album;

    /* Version reached, for the log and the test: 2, 3 or 4. 0 when no tag. */
    uint8_t version;
} nd_id3;

/* Called once per embedded picture, in FILE ORDER, until it returns true.
 *
 * `data` points into the reader's scratch copy of the tag and is valid ONLY
 * for the duration of the call -- copy or decode it there. This shape exists
 * because the Python does not stop at the first APIC frame:
 *
 *     for tag in audio.tags.values():
 *         if isinstance(tag, APIC):
 *             try:    meta["art"] = Image.open(...); break
 *             except: meta["art"] = None
 *
 * The `break` is inside the `try`, so a picture that fails to decode leaves
 * the loop running and the NEXT one is tried. A "return the first picture"
 * interface cannot express that; returning true from here is the `break`. */
typedef bool (*nd_id3_pic_fn)(const uint8_t *data, size_t len, void *ctx);

/* Read `path` -- a LOGICAL path, resolved through nd_path_resolve() like
 * everything else in lib/ -- and fill *out.
 *
 * *out is zeroed first, so a failure still leaves a usable empty tag; the
 * Python's `except Exception` around the whole of get_metadata() means a
 * broken file falls back to the defaults rather than failing the screen, and
 * a caller here can ignore the return value for the same effect.
 *
 * on_pic may be NULL, in which case picture frames are skipped without being
 * copied anywhere.
 *
 *   ND_OK            a tag was found and walked
 *   ND_ERR_NOTFOUND  no "ID3" magic, or a version this does not read
 *   ND_ERR_IO        the file could not be opened or read
 *   ND_ERR_NOMEM     the tag would not fit in memory
 *   ND_ERR_PARSE     the header is self-contradictory (size 0, size > cap) */
nd_err nd_id3_read(const char *path, nd_id3 *out, nd_id3_pic_fn on_pic, void *pic_ctx);

/* The same walk over a tag already in memory, starting at the "ID3" magic.
 * Exposed because a unit test that has to write a file to check a length
 * field is a test of the filesystem. `buf` is not modified; the reader takes
 * its own copy, because removing unsynchronisation rewrites bytes in place. */
nd_err nd_id3_parse(const uint8_t *buf, size_t len, nd_id3 *out, nd_id3_pic_fn on_pic,
                    void *pic_ctx);

/* One ID3 text-frame body -- ENCODING BYTE ALREADY REMOVED -- as UTF-8 in
 * out. Exposed for the test, and because getting this wrong is how a port
 * puts mojibake on a phone.
 *
 *   0  ISO-8859-1   every byte is a codepoint; 0x80..0xFF widen to two bytes
 *   1  UTF-16       with a BOM; a missing BOM is read as little-endian, which
 *                   is what mutagen's codec does
 *   2  UTF-16BE     no BOM
 *   3  UTF-8        copied, truncated only at a codepoint boundary
 *
 * The value stops at the first terminator, so a multi-value frame yields its
 * FIRST value -- mutagen joins them with U+0000, which a C string cannot
 * hold. Returns the number of bytes written, terminator excluded. An unknown
 * encoding writes nothing and returns 0. */
size_t nd_id3_text_utf8(char *out, size_t out_sz, uint8_t encoding, const uint8_t *in, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* ND_ID3_H_INCLUDED */
