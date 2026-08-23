/* nd_text.c -- the six text-fitting routines, kept six.
 *
 * Every function here is a line-by-line port of one Python function, and the
 * differences between them are the whole point of the file. nd_text.h lists
 * them; the short version is that fit_text gives up with "" and _ellipsize
 * gives up with the original string, that one wrapper leaves an over-long
 * word over-wide and two others hard-break it, and that they disagree about
 * whether a trailing blank line survives. Each of those disagreements is
 * visible on at least one screen.
 *
 * ============ WHY THE SCANS RUN FORWARDS ============
 *
 * The Python shortens a string by walking DOWN from its full length and
 * returning the first candidate that fits. Width is monotonic in prefix
 * length -- every advance is non-negative and Python's rstrip() can only
 * remove trailing spaces that a longer prefix puts back -- so "the first
 * candidate that fits, scanning down" and "the last candidate that fits,
 * scanning up" name the same string. Scanning up lets the decoder walk the
 * UTF-8 once, forwards, instead of needing a table of character offsets.
 *
 * ============ NOTHING HERE ALLOCATES ============
 *
 * Candidates are built in a 256-byte stack buffer (ND_TEXT_LINE_MAX), which
 * is comfortably more than the ~48 characters a 240-pixel line of 14 px type
 * holds. A candidate that would not fit that buffer ends the scan; it is
 * already far wider than any screen this OS has.
 */

#include <string.h>

#include "nd_draw.h"
#include "nd_text.h"
#include "nd_ui.h"

/* ------------------------------------------------------------------ *
 * nd_lines
 * ------------------------------------------------------------------ */

void nd_lines_init(nd_lines *l, char (*storage)[ND_TEXT_LINE_MAX], size_t cap)
{
    if (!l)
        return;
    l->buf = storage;
    l->cap = storage ? cap : 0;
    l->n = 0;
    l->truncated = false;
}

void nd_lines_clear(nd_lines *l)
{
    if (!l)
        return;
    l->n = 0;
    l->truncated = false;
}

bool nd_lines_push(nd_lines *l, const char *s)
{
    size_t len;
    bool whole = true;

    if (!l || !l->buf || !s)
        return false;
    if (l->n >= l->cap) {
        l->truncated = true;
        return false;
    }

    len = strlen(s);
    if (len >= ND_TEXT_LINE_MAX) {
        len = ND_TEXT_LINE_MAX - 1;
        l->truncated = true;
        whole = false;
    }
    memcpy(l->buf[l->n], s, len);
    l->buf[l->n][len] = '\0';
    l->n++;
    return whole;
}

const char *nd_lines_at(const nd_lines *l, size_t i)
{
    if (!l || !l->buf || i >= l->n)
        return "";
    return l->buf[i];
}

/* while lines and lines[-1] == "": lines.pop() */
static void lines_pop_trailing_blanks(nd_lines *l)
{
    while (l->n > 0 && l->buf[l->n - 1][0] == '\0')
        l->n--;
}

/* ------------------------------------------------------------------ *
 * Small shared pieces
 * ------------------------------------------------------------------ */

static int32_t text_w(const nd_font *f, const char *s)
{
    int32_t w = 0;

    nd_text_size(f, s, &w, NULL);
    return w;
}

static void set_out(char *out, size_t out_sz, const char *s, size_t len)
{
    if (!out || out_sz == 0)
        return;
    if (len >= out_sz)
        len = out_sz - 1;
    memmove(out, s, len);
    out[len] = '\0';
}

/* Python's str.rstrip() with no argument strips every Unicode whitespace
 * character. Every caller feeds it UI labels, so ASCII whitespace is the
 * whole of the reachable set; the narrowing is recorded in
 * spec-ui-framework.md's "Python idioms" table. */
static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

static size_t rstrip_len(const char *s, size_t len)
{
    while (len > 0 && is_space(s[len - 1]))
        len--;
    return len;
}

/* One source line of text.splitlines(). Python's version also breaks on \v,
 * \f, \x1c-\x1e, \x85, U+2028 and U+2029; none is reachable from any caller
 * and treating them as ordinary characters is the documented narrowing.
 * A trailing separator does NOT produce a final empty line, which is why
 * "a\n" wraps to one line and "a\n\n" to two. */
static const char *raw_line(const char *p, size_t *len, const char **next)
{
    const char *q = p;

    while (*q && *q != '\n' && *q != '\r')
        q++;
    *len = (size_t)(q - p);

    if (*q == '\r' && q[1] == '\n')
        q += 2;
    else if (*q)
        q += 1;
    else
        q = NULL;

    /* Python drops the empty tail after a final separator. */
    if (q && *q == '\0')
        q = NULL;
    *next = q;
    return p;
}

/* The next run of non-space characters in [p, end), i.e. one element of
 * raw.split(" ") with the empty elements already skipped -- which is exactly
 * what "if w == '': continue" does with a run of spaces. */
static bool next_word(const char **p, const char *end, const char **start, size_t *len)
{
    const char *s = *p;
    const char *q;

    while (s < end && *s == ' ')
        s++;
    if (s >= end)
        return false;
    q = s;
    while (q < end && *q != ' ')
        q++;
    *start = s;
    *len = (size_t)(q - s);
    *p = q;
    return true;
}

/* ------------------------------------------------------------------ *
 * fit_text / _ellipsize
 * ------------------------------------------------------------------ */

char *nd_text_fit(char *out, size_t out_sz, const char *text, const nd_font *f, int32_t max_w)
{
    char cand[ND_TEXT_LINE_MAX];
    const char *p;
    size_t best = 0;
    size_t n_chars = 0;
    size_t index = 0;

    if (!out || out_sz == 0)
        return out;
    out[0] = '\0';

    if (max_w <= 0 || !text || !*text)
        return out;

    if (text_w(f, text) <= max_w) {
        set_out(out, out_sz, text, strlen(text));
        return out;
    }

    for (p = text; *p;) {
        (void)nd_utf8_next(&p);
        n_chars++;
    }

    /* range(len(text) - 1, 0, -1): the last character is never a candidate on
     * its own terms -- the full string was already rejected above. */
    for (p = text; *p;) {
        size_t keep;
        size_t off;

        (void)nd_utf8_next(&p);
        index++;
        if (index >= n_chars)
            break;

        off = (size_t)(p - text);
        keep = rstrip_len(text, off);
        if (keep + 3 >= sizeof cand)
            break;
        memcpy(cand, text, keep);
        memcpy(cand + keep, "...", 4);
        if (text_w(f, cand) > max_w)
            break;
        best = keep;
    }

    if (best == 0)
        return out; /* nothing fits -> "" */

    memcpy(cand, text, best);
    memcpy(cand + best, "...", 4);
    set_out(out, out_sz, cand, best + 3);
    return out;
}

char *nd_text_ellipsize(char *out, size_t out_sz, const char *text, const nd_font *f, int32_t max_w)
{
    char cand[ND_TEXT_LINE_MAX];
    const char *p;
    size_t best = 0;
    bool found = false;

    if (!out || out_sz == 0)
        return out;
    out[0] = '\0';
    if (!text)
        return out;

    if (text_w(f, text) <= max_w) {
        set_out(out, out_sz, text, strlen(text));
        return out;
    }

    /* Unlike fit_text this starts from the whole string, and it does not
     * rstrip. */
    for (p = text; *p;) {
        size_t off;

        (void)nd_utf8_next(&p);
        off = (size_t)(p - text);
        if (off + 3 >= sizeof cand)
            break;
        memcpy(cand, text, off);
        memcpy(cand + off, "...", 4);
        if (text_w(f, cand) > max_w)
            break;
        best = off;
        found = true;
    }

    if (!found) {
        /* THE ASYMMETRY WITH fit_text: an empty trim returns the ORIGINAL
         * text, over-wide, rather than "". Two screens rely on it. */
        set_out(out, out_sz, text, strlen(text));
        return out;
    }

    memcpy(cand, text, best);
    memcpy(cand + best, "...", 4);
    set_out(out, out_sz, cand, best + 3);
    return out;
}

/* ------------------------------------------------------------------ *
 * The font ladder
 * ------------------------------------------------------------------ */

size_t nd_font_ladder(const struct nd_ui *ui, const nd_font *out[], size_t max)
{
    const nd_font *want[3];
    size_t n = 0;
    size_t i;
    size_t j;

    if (!ui || !out || max == 0)
        return 0;

    /* _font_ladder(ui, "font_n", "font_md", "font_s") -- the only call in the
     * shipped code, and the reason this takes no name list. */
    want[0] = ui->font_n;
    want[1] = ui->font_md;
    want[2] = ui->font_s;

    for (i = 0; i < ND_ARRAY_LEN(want); i++) {
        bool dup = false;

        if (!want[i])
            continue;
        /* "font not in fonts": a UI whose font_md failed to load holds font_n
         * twice and the ladder must not offer the same size two rungs
         * running. */
        for (j = 0; j < n; j++) {
            if (out[j] == want[i])
                dup = true;
        }
        if (dup)
            continue;
        out[n++] = want[i];
        if (n == max)
            break;
    }
    return n;
}

const nd_font *nd_fit_font(const char *text, int32_t max_w, const nd_font *const *fonts,
                           size_t n_fonts)
{
    size_t i;

    if (!fonts || n_fonts == 0)
        return NULL;
    for (i = 0; i < n_fonts; i++) {
        if (text_w(fonts[i], text) <= max_w)
            return fonts[i];
    }
    return fonts[n_fonts - 1];
}

/* ------------------------------------------------------------------ *
 * Wrapping
 * ------------------------------------------------------------------ */

void nd_text_wrap(nd_lines *out, const char *text, const nd_font *f, int32_t max_w)
{
    char cur[ND_TEXT_LINE_MAX];
    char cand[ND_TEXT_LINE_MAX];
    const char *raw;
    const char *next;
    size_t raw_len;

    if (!out)
        return;
    nd_lines_clear(out);
    if (!text)
        text = "";

    raw = raw_line(text, &raw_len, &next);
    for (;;) {
        const char *p = raw;
        const char *end = raw + raw_len;
        const char *word = NULL;
        size_t n = 0;
        size_t wlen = 0;

        cur[0] = '\0';

        /* "if not raw.strip(): lines.append(''); continue" -- a line of only
         * whitespace becomes a blank line rather than a line holding a tab.
         * The hard-breaking wrappers have no such shortcut and therefore
         * differ for a tab-only line. */
        {
            size_t k = 0;
            bool blank = true;

            while (k < raw_len) {
                if (!is_space(raw[k])) {
                    blank = false;
                    break;
                }
                k++;
            }
            if (blank) {
                nd_lines_push(out, "");
                goto next_raw;
            }
        }

        while (next_word(&p, end, &word, &wlen)) {
            size_t cand_n;

            if (n == 0) {
                /* candidate == word */
                memcpy(cand, word, wlen < sizeof cand ? wlen : sizeof cand - 1);
                cand_n = wlen < sizeof cand ? wlen : sizeof cand - 1;
                cand[cand_n] = '\0';
                /* "or not current": the first word goes on the line however
                 * wide it is, which is what leaves a long word over-wide. */
                memcpy(cur, cand, cand_n + 1);
                n = cand_n;
                continue;
            }

            memcpy(cand, cur, n);
            cand_n = n;
            if (cand_n + 1 + wlen >= sizeof cand) {
                nd_lines_push(out, cur);
                memcpy(cur, word, wlen < sizeof cur ? wlen : sizeof cur - 1);
                n = wlen < sizeof cur ? wlen : sizeof cur - 1;
                cur[n] = '\0';
                continue;
            }
            cand[cand_n++] = ' ';
            memcpy(cand + cand_n, word, wlen);
            cand_n += wlen;
            cand[cand_n] = '\0';

            if (text_w(f, cand) <= max_w) {
                memcpy(cur, cand, cand_n + 1);
                n = cand_n;
            } else {
                nd_lines_push(out, cur);
                memcpy(cur, word, wlen);
                n = wlen;
                cur[n] = '\0';
            }
        }
        nd_lines_push(out, cur);

    next_raw:
        if (!next)
            break;
        raw = raw_line(next, &raw_len, &next);
    }

    lines_pop_trailing_blanks(out);
}

/* break_long_word(): accumulate characters until one more would overflow
 * max_w, then start a new piece. Shared verbatim by TextInputLong and
 * MessageDialog. */
static void break_long_word(nd_lines *out, const char *word, size_t wlen, const nd_font *f,
                            int32_t max_w)
{
    char cur[ND_TEXT_LINE_MAX];
    char nxt[ND_TEXT_LINE_MAX];
    const char *p = word;
    const char *end = word + wlen;
    size_t n = 0;
    size_t pushed = 0;

    cur[0] = '\0';
    while (p < end) {
        const char *q = p;
        size_t clen;

        (void)nd_utf8_next(&q);
        if (q > end)
            q = end;
        clen = (size_t)(q - p);

        if (n + clen >= sizeof nxt) {
            if (n > 0) {
                nd_lines_push(out, cur);
                pushed++;
            }
            n = clen < sizeof cur ? clen : sizeof cur - 1;
            memcpy(cur, p, n);
            cur[n] = '\0';
            p = q;
            continue;
        }

        memcpy(nxt, cur, n);
        memcpy(nxt + n, p, clen);
        nxt[n + clen] = '\0';

        if (n > 0 && text_w(f, nxt) > max_w) {
            nd_lines_push(out, cur);
            pushed++;
            memcpy(cur, p, clen);
            n = clen;
            cur[n] = '\0';
        } else {
            memcpy(cur, nxt, n + clen + 1);
            n += clen;
        }
        p = q;
    }

    if (n > 0) {
        nd_lines_push(out, cur);
        pushed++;
    }
    if (pushed == 0)
        nd_lines_push(out, ""); /* "return out or [word]" with word empty */
}

static void wrap_hard(nd_lines *out, const char *text, const nd_font *f, int32_t max_w,
                      bool pop_blanks)
{
    char cur[ND_TEXT_LINE_MAX];
    char cand[ND_TEXT_LINE_MAX];
    char word_z[ND_TEXT_LINE_MAX];
    const char *raw;
    const char *next;
    size_t raw_len;

    if (!out)
        return;
    nd_lines_clear(out);
    if (!text)
        text = "";

    raw = raw_line(text, &raw_len, &next);
    for (;;) {
        const char *p = raw;
        const char *end = raw + raw_len;
        const char *word = NULL;
        size_t n = 0;
        size_t wlen = 0;

        cur[0] = '\0';

        while (next_word(&p, end, &word, &wlen)) {
            size_t wz = wlen < sizeof word_z ? wlen : sizeof word_z - 1;
            size_t cand_n;

            memcpy(word_z, word, wz);
            word_z[wz] = '\0';

            /* A word wider than the line is broken between characters here,
             * which is the one thing _wrap_lines refuses to do. */
            if (text_w(f, word_z) > max_w) {
                if (n > 0) {
                    nd_lines_push(out, cur);
                    n = 0;
                    cur[0] = '\0';
                }
                break_long_word(out, word, wlen, f, max_w);
                continue;
            }

            if (n == 0) {
                memcpy(cur, word_z, wz + 1);
                n = wz;
                continue;
            }

            memcpy(cand, cur, n);
            cand_n = n;
            if (cand_n + 1 + wz >= sizeof cand) {
                nd_lines_push(out, cur);
                memcpy(cur, word_z, wz + 1);
                n = wz;
                continue;
            }
            cand[cand_n++] = ' ';
            memcpy(cand + cand_n, word_z, wz + 1);
            cand_n += wz;

            if (text_w(f, cand) <= max_w) {
                memcpy(cur, cand, cand_n + 1);
                n = cand_n;
            } else {
                nd_lines_push(out, cur);
                memcpy(cur, word_z, wz + 1);
                n = wz;
            }
        }
        nd_lines_push(out, cur);

        if (!next)
            break;
        raw = raw_line(next, &raw_len, &next);
    }

    if (pop_blanks)
        lines_pop_trailing_blanks(out);
    else if (out->n == 0)
        nd_lines_push(out, ""); /* "if not lines: return ['']" */
}

void nd_text_wrap_break(nd_lines *out, const char *text, const nd_font *f, int32_t max_w)
{
    wrap_hard(out, text, f, max_w, false);
}

void nd_text_wrap_break_pop(nd_lines *out, const char *text, const nd_font *f, int32_t max_w)
{
    wrap_hard(out, text, f, max_w, true);
}

/* ------------------------------------------------------------------ *
 * Decoration
 * ------------------------------------------------------------------ */

void nd_underline_tail(struct nd_draw *d, int32_t x, int32_t y, const char *line, size_t tail_len,
                       const nd_font *f)
{
    char head[ND_TEXT_LINE_MAX];
    size_t len;
    int32_t head_w = 0;
    int32_t full_w = 0;
    int32_t height = 0;
    int32_t rule_y;

    if (!d || !line || tail_len == 0)
        return;
    len = strlen(line);
    if (len < tail_len)
        return;

    if (len > tail_len) {
        size_t keep = len - tail_len;

        if (keep >= sizeof head)
            keep = sizeof head - 1;
        memcpy(head, line, keep);
        head[keep] = '\0';
        head_w = text_w(f, head);
    }

    nd_text_size(f, line, &full_w, &height);
    /* The rule sits one pixel below the INK box of the whole line, so it
     * moves depending on whether this line happens to contain a descender.
     * That is what the phone does today; do not stabilise it against the
     * font's line height. */
    rule_y = y + height + 1;
    (void)nd_draw_line(d, x + head_w, rule_y, x + full_w, rule_y, ND_WHITE, 1);
}
