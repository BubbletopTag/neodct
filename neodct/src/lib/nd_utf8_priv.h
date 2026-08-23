/* nd_utf8_priv.h -- UTF-8 validation and replacement, shared inside libneodct.
 *
 * NOT a public header. It is here because two unrelated modules need exactly
 * the same decoder and a second copy would drift:
 *
 *   nd_props.c  settings.prop and version.prop are decoded STRICT, so a file
 *               with a stray 0xFF reads as empty rather than as garbage --
 *               test_settings_version_layering.py:87 pins that.
 *               /run/neodct/sdcard.prop is decoded with REPLACEMENT, so a
 *               corrupt file still yields whatever lines survived.
 *
 *   nd_json.c   json.loads() on bytes decodes strict UTF-8 before it parses,
 *               so a document that is not valid UTF-8 is rejected before the
 *               grammar is ever consulted.
 *
 * "Strict" here means what CPython's utf-8 codec means: no overlong forms, no
 * surrogates (U+D800..U+DFFF), nothing above U+10FFFF, no truncated tails.
 */

#ifndef ND_UTF8_PRIV_H_INCLUDED
#define ND_UTF8_PRIV_H_INCLUDED

#include "nd_types.h"

/* True when the whole buffer decodes as strict UTF-8. An empty buffer is
 * valid. Embedded NULs are valid -- Python decodes b"\x00" happily; it is the
 * 0xFF beside it in the test fixture that does the rejecting. */
bool nd_utf8_valid(const uint8_t *data, size_t len);

/* Decode with errors="replace": every maximal invalid subpart becomes one
 * U+FFFD, which is the rule CPython follows. Writes at most out_sz bytes
 * including the terminating NUL and returns the number of bytes written
 * excluding it, or (size_t)-1 if it would not fit.
 *
 * Worst case growth is 3x (each invalid byte becomes a 3-byte U+FFFD), so a
 * caller sizing the output at 3 * len + 1 can never see the failure. */
size_t nd_utf8_replace(const uint8_t *data, size_t len, char *out, size_t out_sz);

#endif /* ND_UTF8_PRIV_H_INCLUDED */
