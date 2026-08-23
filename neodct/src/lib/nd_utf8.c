/* nd_utf8.c -- a strict UTF-8 decoder, and the same decoder in the forgiving
 * mode Python spells errors="replace".
 *
 * Written out longhand rather than pulled from a table because the exact
 * rejection rules are load-bearing: settings.prop is decoded strict and a
 * corrupt one must read as EMPTY, not as "whatever bytes happened to parse".
 * A decoder that quietly accepted overlong forms or lone surrogates would
 * pass every test in the suite and still be wrong on the one file that
 * matters.
 */

#include <string.h>

#include "nd_utf8_priv.h"

/* Length of the sequence a lead byte announces, or 0 if it cannot start one.
 * 0xC0 and 0xC1 are excluded here rather than caught later: they can only ever
 * introduce an overlong two-byte encoding. */
static size_t lead_len(uint8_t c)
{
    if (c < 0x80u)
        return 1u;
    if (c >= 0xC2u && c <= 0xDFu)
        return 2u;
    if (c >= 0xE0u && c <= 0xEFu)
        return 3u;
    if (c >= 0xF0u && c <= 0xF4u)
        return 4u;
    return 0u;
}

/* How many bytes of a would-be sequence at data[0..len) are actually well
 * formed. Returns the sequence length on success, and on failure returns 0
 * with *bad set to the length of the maximal invalid subpart -- which is what
 * errors="replace" collapses into a single U+FFFD. */
static size_t seq_len(const uint8_t *data, size_t len, size_t *bad)
{
    size_t need = lead_len(data[0]);
    uint8_t lo = 0x80u;
    uint8_t hi = 0xBFu;
    size_t i;

    if (need == 0u) {
        *bad = 1u;
        return 0u;
    }
    if (need == 1u)
        return 1u;

    /* The second byte carries the range restrictions that rule out overlong
     * forms, the surrogate block and anything past U+10FFFF. */
    switch (data[0]) {
    case 0xE0u:
        lo = 0xA0u;
        break; /* else overlong                */
    case 0xEDu:
        hi = 0x9Fu;
        break; /* else U+D800..U+DFFF          */
    case 0xF0u:
        lo = 0x90u;
        break; /* else overlong                */
    case 0xF4u:
        hi = 0x8Fu;
        break; /* else beyond U+10FFFF         */
    default:
        break;
    }

    for (i = 1u; i < need; i++) {
        uint8_t c;

        if (i >= len) {
            /* Truncated at the end of the buffer: the bytes seen so far are
             * the maximal subpart. */
            *bad = len;
            return 0u;
        }
        c = data[i];
        if (i == 1u) {
            if (c < lo || c > hi) {
                *bad = 1u;
                return 0u;
            }
        } else if (c < 0x80u || c > 0xBFu) {
            *bad = i;
            return 0u;
        }
    }

    return need;
}

bool nd_utf8_valid(const uint8_t *data, size_t len)
{
    size_t i = 0u;

    if (data == NULL)
        return len == 0u;

    while (i < len) {
        size_t bad = 0u;
        size_t n = seq_len(data + i, len - i, &bad);

        if (n == 0u)
            return false;
        i += n;
    }
    return true;
}

size_t nd_utf8_replace(const uint8_t *data, size_t len, char *out, size_t out_sz)
{
    static const char repl[3] = {(char)0xEFu, (char)0xBFu, (char)0xBDu}; /* U+FFFD */
    size_t i = 0u;
    size_t w = 0u;

    if (out == NULL || out_sz == 0u)
        return (size_t)-1;

    while (i < len) {
        size_t bad = 0u;
        size_t n = seq_len(data + i, len - i, &bad);

        if (n > 0u) {
            if (w + n >= out_sz)
                return (size_t)-1;
            memcpy(out + w, data + i, n);
            w += n;
            i += n;
        } else {
            if (w + sizeof repl >= out_sz)
                return (size_t)-1;
            memcpy(out + w, repl, sizeof repl);
            w += sizeof repl;
            i += bad > 0u ? bad : 1u;
        }
    }

    out[w] = '\0';
    return w;
}
