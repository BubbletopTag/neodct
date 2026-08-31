/* nd_util.c -- the four string helpers and the two numeric ones declared in
 * nd_types.h, which is the only header that has no module of its own.
 *
 * These are here rather than as static inlines in the header because every
 * process in the system links libneodct anyway, and one copy in the shared
 * mapping costs less than a copy inlined into each of thirty call sites on a
 * device where text pages are the thing we are short of.
 */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nd_types.h"

const char *nd_strerror(nd_err err)
{
    switch (err) {
    case ND_OK:
        return "ok";
    case ND_ERR_NOMEM:
        return "out of memory";
    case ND_ERR_IO:
        return "I/O error";
    case ND_ERR_INVAL:
        return "invalid argument";
    case ND_ERR_NOTFOUND:
        return "not found";
    case ND_ERR_TOOLONG:
        return "too long";
    case ND_ERR_HARDWARE:
        return "hardware error";
    case ND_ERR_BUSY:
        return "busy";
    case ND_ERR_TIMEOUT:
        return "timed out";
    case ND_ERR_PARSE:
        return "malformed";
    case ND_ERR_UNSUPPORTED:
        return "unsupported";
    case ND_ERR_PERM:
        return "refused: cannot be done without privilege this must not have";
    }

    /* Not a default: label, so -Wswitch still catches a new enumerator. */
    return "unknown error";
}

size_t nd_strlcpy(char *dst, const char *src, size_t dst_sz)
{
    size_t srclen = strlen(src);

    if (dst_sz > 0u) {
        size_t copy = srclen < dst_sz - 1u ? srclen : dst_sz - 1u;

        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return srclen;
}

size_t nd_strlcat(char *dst, const char *src, size_t dst_sz)
{
    size_t dstlen = 0u;
    size_t srclen = strlen(src);

    while (dstlen < dst_sz && dst[dstlen] != '\0')
        dstlen++;

    /* An unterminated dst has no room left by definition; BSD returns
     * dst_sz + strlen(src) so the truncation test still fires. */
    if (dstlen == dst_sz)
        return dst_sz + srclen;

    (void)nd_strlcpy(dst + dstlen, src, dst_sz - dstlen);
    return dstlen + srclen;
}

nd_err nd_snprintf(char *dst, size_t dst_sz, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (dst == NULL || dst_sz == 0u)
        return ND_ERR_INVAL;

    va_start(ap, fmt);
    n = vsnprintf(dst, dst_sz, fmt, ap);
    va_end(ap);

    if (n < 0)
        return ND_ERR_INVAL;
    if ((size_t)n >= dst_sz)
        return ND_ERR_TOOLONG;

    return ND_OK;
}

double nd_round_half_even(double v)
{
    double base;
    double frac;

    /* NaN and the infinities round to themselves, as Python's round() does
     * for floats (it raises for those, but no call site can produce one and
     * propagating is more useful than trapping in a drawing routine). */
    if (!isfinite(v))
        return v;

    base = floor(v);
    frac = v - base;

    if (frac > 0.5)
        return base + 1.0;
    if (frac < 0.5)
        return base;

    /* Exactly halfway: go to the even neighbour. fmod keeps the sign of base,
     * so -3.0 gives -1.0 and is correctly judged odd. */
    return (fmod(base, 2.0) == 0.0) ? base : base + 1.0;
}
