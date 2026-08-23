/* nd_t9_dict.c -- prefix lookup over the on-disk T9 word list.
 *
 * Ported from System/hw/t9_dict.py, and the reason that file exists at all is
 * the reason this one does: the shipped dictionary is 2.88 MiB and 315,752
 * words, and it is NEVER LOADED INTO RAM. One descriptor, no cache, no
 * resident copy, about seventeen pread()s per lookup. On a phone with 53 MB
 * that is not an optimisation, it is the only way the feature can exist.
 *
 * So: pread the middle, find the line you landed in, compute its digit
 * sequence, compare, halve. The file holds no digits precisely because they
 * are recomputable, and recomputing one is cheaper than storing 315,752.
 *
 * ============ THINGS THAT LOOK WRONG AND ARE NOT ============
 *
 *   - The back-scan chunk INCLUDES the byte at `offset`. If that byte is
 *     itself '\n', the line found is the NEXT one. The Python does this and
 *     the search's progress guard is tuned around it.
 *   - Comparison is byte-wise strcmp on digit characters, not numeric. "9"
 *     sorts after "10", which is what the builder sorted the file by.
 *   - An untypeable line (the shipped file has exactly one word with
 *     capitals, and digits_for is case-insensitive, so in practice none)
 *     keys to "" and therefore sorts first. Reproduced.
 *
 * The module docstring in the Python says "half a megabyte" and "76,000
 * words". That is stale -- the shipped file was built with a larger budget.
 * Nothing here is sized from it.
 */

#include "nd_t9.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_paths.h"

/* Bytes to reach back for the start of the line a probe landed in. Words are
 * short, so this is a handful of bytes rather than a scan. */
#define BACKSCAN 64

/* Longest line this reader will look at. The builder caps words at 12, so
 * this is twenty times the slack needed; it exists because
 * CODING-STANDARDS.md section 1.5 does not allow a read sized by the file.
 * A longer line comes back truncated, which the Python would not do -- see
 * OPEN-QUESTIONS.md. */
#define LINE_MAX 255

struct nd_t9_dict {
    int fd;
    off_t size;
};

/* Strip ASCII whitespace from both ends and drop non-ASCII bytes, which is
 * bytes.strip() followed by .decode("ascii", "ignore"). */
static void strip_ascii(const char *raw, size_t raw_len, char *out, size_t out_sz)
{
    size_t begin = 0u;
    size_t end = raw_len;
    size_t n = 0u;
    size_t i;

    while (begin < end && isspace((unsigned char)raw[begin]) != 0)
        begin++;
    while (end > begin && isspace((unsigned char)raw[end - 1u]) != 0)
        end--;

    for (i = begin; i < end && n + 1u < out_sz; i++) {
        unsigned char c = (unsigned char)raw[i];

        if (c < 0x80u)
            out[n++] = (char)c;
    }
    out[n] = '\0';
}

/* Read the line beginning at `start`. Returns its stripped word in `word` and
 * the number of raw bytes consumed (line plus its newline) in `consumed`.
 * Returns false at end of file, which is Python's `if not raw: break`. */
static bool read_line_at(const nd_t9_dict *d, off_t start, char *word, size_t word_sz,
                         size_t *consumed)
{
    char raw[LINE_MAX + 1];
    ssize_t got;
    const char *nl;
    size_t line_len;

    if (start < 0 || start >= d->size)
        return false;

    got = pread(d->fd, raw, sizeof raw, start);
    if (got <= 0)
        return false;

    nl = memchr(raw, '\n', (size_t)got);
    line_len = (nl != NULL) ? (size_t)(nl - raw) : (size_t)got;

    strip_ascii(raw, line_len, word, word_sz);
    *consumed = (nl != NULL) ? line_len + 1u : (size_t)got;
    return true;
}

/* _line_at(offset) -> (start, word), byte for byte. */
static void line_at(const nd_t9_dict *d, off_t offset, off_t *start_out, char *word, size_t word_sz)
{
    char chunk[BACKSCAN + 1];
    off_t back = (offset > (off_t)BACKSCAN) ? offset - (off_t)BACKSCAN : 0;
    off_t start = back;
    size_t consumed;

    word[0] = '\0';

    if (offset > back) {
        /* The chunk is [back, offset] INCLUSIVE -- offset - back + 1 bytes. */
        size_t want = (size_t)(offset - back) + 1u;
        ssize_t got;

        if (want > sizeof chunk)
            want = sizeof chunk;
        got = pread(d->fd, chunk, want, back);
        if (got > 0) {
            ssize_t i;

            for (i = got - 1; i >= 0; i--) {
                if (chunk[i] == '\n') {
                    start = back + i + 1;
                    break;
                }
            }
        }
    }

    *start_out = start;
    (void)read_line_at(d, start, word, word_sz, &consumed);
}

/* digits_for() over a buffer whose size the caller vouches for. */
static bool digits_for_buf(const char *word, char *out, size_t out_sz)
{
    /* Same mapping as LETTER_CYCLES, inverted. Kept local rather than shared
     * with the engine so a broken dictionary cannot take multi-tap with it --
     * that separation is deliberate in the Python and worth keeping. */
    static const char *const groups[8] = {"abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz"};
    size_t n = 0u;
    size_t i;

    if (word == NULL || out_sz == 0u)
        return false;

    for (i = 0u; word[i] != '\0'; i++) {
        char lower = (char)tolower((unsigned char)word[i]);
        size_t g;
        bool hit = false;

        for (g = 0u; g < 8u && !hit; g++) {
            if (strchr(groups[g], lower) != NULL) {
                if (n + 1u >= out_sz)
                    return false;
                out[n++] = (char)('2' + (int)g);
                hit = true;
            }
        }
        if (!hit)
            return false; /* punctuation, a digit, a space: not typeable */
    }
    out[n] = '\0';
    return true;
}

bool nd_t9_digits_for(const char *word, char *out, size_t out_sz)
{
    return digits_for_buf(word, out, out_sz);
}

/* Deliberately NOT nd_path_resolve()d: this takes a real filesystem path so
 * a test can point it at a fixture and the wizard can point it anywhere.
 * nd_t9_dict_shared() is where the ND_ROOT-relative constant is resolved. */
nd_t9_dict *nd_t9_dict_open(const char *path)
{
    /* owned by the caller; free with nd_t9_dict_close() */
    nd_t9_dict *d;
    struct stat st;

    if (path == NULL)
        return NULL;

    d = calloc(1u, sizeof *d);
    if (d == NULL)
        return NULL;

    d->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (d->fd < 0) {
        /* No dictionary is NOT an error: the phone falls back to multi-tap,
         * which is what it did before this existed. The object stays valid
         * and simply reports itself unavailable. */
        d->size = 0;
        return d;
    }
    if (fstat(d->fd, &st) != 0) {
        (void)close(d->fd);
        d->fd = -1;
        d->size = 0;
        return d;
    }
    d->size = st.st_size;
    return d;
}

void nd_t9_dict_close(nd_t9_dict *d)
{
    if (d == NULL)
        return;
    if (d->fd >= 0)
        (void)close(d->fd);
    free(d);
}

bool nd_t9_dict_available(const nd_t9_dict *d)
{
    return d != NULL && d->fd >= 0 && d->size > 0;
}

static nd_t9_dict *g_shared;
static pthread_once_t g_shared_once = PTHREAD_ONCE_INIT;

static void shared_init(void)
{
    char resolved[ND_PATH_MAX];

    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_T9_DICT) != ND_OK)
        return;
    g_shared = nd_t9_dict_open(resolved);
}

nd_t9_dict *nd_t9_dict_shared(void)
{
    /* One descriptor for the life of the process is the entire memory cost,
     * and the file is read-only, so every process opening its own is cheaper
     * than any IPC would be. pthread_once because the core is threaded. */
    (void)pthread_once(&g_shared_once, shared_init);
    return g_shared;
}

size_t nd_t9_dict_suggest(nd_t9_dict *d, const char *digits, char out[][ND_T9_WORD_MAX],
                          size_t limit)
{
    char word[LINE_MAX + 1];
    char key[LINE_MAX + 1];
    size_t digit_len;
    size_t i;
    size_t n = 0u;
    off_t low;
    off_t high;
    off_t pos;

    if (!nd_t9_dict_available(d) || digits == NULL || out == NULL || limit == 0u)
        return 0u;

    digit_len = strlen(digits);
    if (digit_len < (size_t)ND_T9_MIN_PREFIX)
        return 0u; /* one digit matches thousands; multi-tap is the answer */
    for (i = 0u; i < digit_len; i++) {
        if (digits[i] < '2' || digits[i] > '9')
            return 0u;
    }

    /* Binary search for the first line whose key is >= digits. */
    low = 0;
    high = d->size;
    while (low < high) {
        off_t mid = low + (high - low) / 2;
        off_t start;

        line_at(d, mid, &start, word, sizeof word);
        if (start <= low && (high - low) <= 1)
            break; /* progress guard: the probe cannot move any further */

        if (!digits_for_buf(word, key, sizeof key))
            key[0] = '\0'; /* an untypeable line sorts first */

        if (strcmp(key, digits) < 0)
            low = start + (off_t)strlen(word) + 1;
        else
            high = start;
    }

    pos = low;
    while (n < limit) {
        size_t consumed;
        int cmp;

        if (!read_line_at(d, pos, word, sizeof word, &consumed))
            break; /* end of file */
        pos += (off_t)consumed;

        if (word[0] == '\0')
            continue;
        if (!digits_for_buf(word, key, sizeof key))
            continue;

        cmp = strncmp(key, digits, digit_len);
        if (cmp != 0) {
            /* Sorted file: the first key past the run means the run is over
             * and nothing further can match. */
            if (cmp > 0)
                break;
            continue;
        }
        if (strlen(word) >= (size_t)ND_T9_WORD_MAX)
            continue; /* cannot fit the caller's slot; the builder caps at 12 */

        (void)nd_strlcpy(out[n], word, (size_t)ND_T9_WORD_MAX);
        n++;
    }
    return n;
}
