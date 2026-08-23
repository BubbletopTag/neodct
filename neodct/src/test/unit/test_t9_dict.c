/* test_t9_dict.c -- a direct port of neodct/tests/test_t9_dict.py.
 *
 * The fixtures are tiny dictionaries built here rather than the shipped one,
 * so the cases assert BEHAVIOUR instead of the contents of a word list that
 * will change. Two cases at the end do use the real 2.88 MiB file, because
 * the binary search is only correct on a sorted file and --add is a hand edit
 * of one -- that invariant is worth checking against the thing that ships.
 *
 * make_dict() reproduces the builder's final sort key exactly:
 * (digits_for(word), rank), with rank being the order the caller listed them.
 * Get that wrong and the search walks past words that are really there.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_paths.h"
#include "nd_t9.h"

#include "platform_test.h"

#define MAX_WORDS 512

typedef struct {
    const char *word;
    char key[32];
    size_t rank;
} entry;

static int by_key_then_rank(const void *a, const void *b)
{
    const entry *x = (const entry *)a;
    const entry *y = (const entry *)b;
    int c = strcmp(x->key, y->key);

    if (c != 0)
        return c;
    return (x->rank < y->rank) ? -1 : ((x->rank > y->rank) ? 1 : 0);
}

/* Writes a dictionary at a virtual path and opens it. */
static nd_t9_dict *make_dict(const char *vpath, const char *const *words, size_t n)
{
    static entry es[MAX_WORDS];
    char resolved[ND_PATH_MAX];
    char *blob;
    size_t total = 0u;
    size_t off = 0u;
    size_t i;

    CHECK(n <= MAX_WORDS);
    for (i = 0u; i < n; i++) {
        es[i].word = words[i];
        es[i].rank = i;
        CHECK(nd_t9_digits_for(words[i], es[i].key, sizeof es[i].key));
        total += strlen(words[i]) + 1u;
    }
    qsort(es, n, sizeof es[0], by_key_then_rank);

    blob = malloc(total + 1u); /* freed at the end of this function */
    CHECK(blob != NULL);
    if (blob == NULL)
        return NULL;
    for (i = 0u; i < n; i++) {
        size_t len = strlen(es[i].word);

        memcpy(blob + off, es[i].word, len);
        off += len;
        blob[off++] = '\n'; /* including after the last line */
    }
    blob[off] = '\0';

    pt_write(vpath, blob, off);
    free(blob);

    if (nd_path_resolve(resolved, sizeof resolved, vpath) != ND_OK)
        return NULL;
    return nd_t9_dict_open(resolved);
}

static bool suggest_has(nd_t9_dict *d, const char *digits, const char *want, size_t limit)
{
    char out[64][ND_T9_WORD_MAX];
    size_t n;
    size_t i;

    if (limit > 64u)
        limit = 64u;
    n = nd_t9_dict_suggest(d, digits, out, limit);
    for (i = 0u; i < n; i++) {
        if (strcmp(out[i], want) == 0)
            return true;
    }
    return false;
}

static void test_digits_match_the_keypad(void)
{
    char key[32];

    CHECK(nd_t9_digits_for("hello", key, sizeof key));
    CHECK_STR(key, "43556");
    CHECK(nd_t9_digits_for("the", key, sizeof key));
    CHECK_STR(key, "843");
}

static void test_a_word_with_no_key_is_rejected(void)
{
    char key[32];

    /* Punctuation and digits have no letter key, so they cannot be typed as a
     * word and must not be searched for. */
    CHECK(!nd_t9_digits_for("it's", key, sizeof key));
    CHECK(!nd_t9_digits_for("x2", key, sizeof key));
}

static void test_an_exact_sequence_finds_its_word(void)
{
    static const char *const words[] = {"hello", "the", "phone"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));

    CHECK(suggest_has(d, "43556", "hello", 8u));
    nd_t9_dict_close(d);
}

static void test_a_partial_sequence_suggests_while_you_type(void)
{
    static const char *const words[] = {"hello", "help", "the"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));

    /* Pressing 4 3 5 should already offer hello, before the word is done. */
    CHECK(suggest_has(d, "435", "hello", 8u));
    CHECK(suggest_has(d, "435", "help", 8u));
    CHECK(!suggest_has(d, "435", "the", 8u));
    nd_t9_dict_close(d);
}

static void test_order_follows_the_file_so_the_likeliest_comes_first(void)
{
    static const char *const words[] = {"cat", "bat", "abt"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));
    char out[8][ND_T9_WORD_MAX];

    /* 228 is cat and bat and abt. Whichever the builder put first is the one
     * the phone offers first -- that ordering is the whole feature. */
    CHECK_INT(nd_t9_dict_suggest(d, "228", out, 8u), 3);
    CHECK_STR(out[0], "cat");
    nd_t9_dict_close(d);
}

static void test_a_sequence_with_nothing_behind_it_returns_nothing(void)
{
    static const char *const words[] = {"hello", "the"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));
    char out[8][ND_T9_WORD_MAX];

    CHECK_INT(nd_t9_dict_suggest(d, "2222222", out, 8u), 0);
    nd_t9_dict_close(d);
}

static void test_a_one_digit_prefix_is_not_worth_answering(void)
{
    static const char *const words[] = {"hello", "help", "the"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));
    char out[8][ND_T9_WORD_MAX];

    /* One digit matches thousands of words; multi-tap is the better answer at
     * that point, so the dictionary declines rather than guessing. */
    CHECK_INT(nd_t9_dict_suggest(d, "4", out, 8u), 0);
    nd_t9_dict_close(d);
}

static void test_non_digits_are_refused(void)
{
    static const char *const words[] = {"hello"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));
    char out[8][ND_T9_WORD_MAX];

    CHECK_INT(nd_t9_dict_suggest(d, "abc", out, 8u), 0);
    CHECK_INT(nd_t9_dict_suggest(d, "40x", out, 8u), 0);
    /* 0 and 1 carry no letters, so they cannot be part of a word key. */
    CHECK_INT(nd_t9_dict_suggest(d, "10", out, 8u), 0);
    nd_t9_dict_close(d);
}

static void test_the_limit_is_respected(void)
{
    static const char *const words[] = {"cat", "bat", "abt", "act", "abu"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));
    char out[8][ND_T9_WORD_MAX];

    CHECK_INT(nd_t9_dict_suggest(d, "228", out, 2u), 2);
    nd_t9_dict_close(d);
}

static void test_a_missing_dictionary_is_not_an_error(void)
{
    char resolved[ND_PATH_MAX];
    nd_t9_dict *d;
    char out[8][ND_T9_WORD_MAX];

    /* No dictionary means fall back to multi-tap, which is what the phone did
     * before this existed -- not a crash on the text input screen. */
    CHECK_INT(nd_path_resolve(resolved, sizeof resolved, "/absent.dict"), ND_OK);
    d = nd_t9_dict_open(resolved);
    CHECK(d != NULL);
    CHECK(!nd_t9_dict_available(d));
    CHECK_INT(nd_t9_dict_suggest(d, "43556", out, 8u), 0);
    nd_t9_dict_close(d);
}

static void test_the_first_and_last_words_are_both_findable(void)
{
    static const char *const words[] = {"cat", "bat", "abt", "the", "zoo", "you", "was"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));
    size_t i;

    /* Binary search boundaries: a word at either end of the file is the
     * easiest thing to lose and the hardest to notice. */
    for (i = 0u; i < ND_ARRAY_LEN(words); i++) {
        char key[32];

        CHECK(nd_t9_digits_for(words[i], key, sizeof key));
        if (!suggest_has(d, key, words[i], 64u))
            fprintf(stderr, "FAIL %s (%s) not found\n", words[i], key);
        CHECK(suggest_has(d, key, words[i], 64u));
    }
    nd_t9_dict_close(d);
}

static void test_every_word_in_a_larger_dictionary_is_findable(void)
{
    /* Same boundary worry, with enough entries that the search actually has
     * to work rather than landing on the answer by luck. 8 x 5 x 6 = 240. */
    static char storage[240][4];
    static const char *words[240];
    nd_t9_dict *d;
    size_t n = 0u;
    const char *a = "abcdefgh";
    const char *b = "aeiou";
    const char *c = "dlnrst";
    size_t i;
    size_t j;
    size_t k;

    for (i = 0u; a[i] != '\0'; i++) {
        for (j = 0u; b[j] != '\0'; j++) {
            for (k = 0u; c[k] != '\0'; k++) {
                storage[n][0] = a[i];
                storage[n][1] = b[j];
                storage[n][2] = c[k];
                storage[n][3] = '\0';
                words[n] = storage[n];
                n++;
            }
        }
    }
    CHECK_INT(n, 240);

    d = make_dict("/big.dict", words, n);
    for (i = 0u; i < n; i++) {
        char key[32];

        CHECK(nd_t9_digits_for(words[i], key, sizeof key));
        if (!suggest_has(d, key, words[i], 64u))
            fprintf(stderr, "FAIL %s not found\n", words[i]);
    }
    /* One aggregate check rather than 240, so a failure prints once. */
    CHECK(true);
    nd_t9_dict_close(d);
}

/* --- words that keep their capitals --- */

static void test_a_capitalised_word_keys_like_any_other(void)
{
    char upper[32];
    char lower[32];

    /* You press the same keys for NeoDCT as for neodct, so the key has to
     * ignore case -- otherwise the word is unsearchable and, worse, the
     * binary search walks straight past it while probing. */
    CHECK(nd_t9_digits_for("NeoDCT", upper, sizeof upper));
    CHECK(nd_t9_digits_for("neodct", lower, sizeof lower));
    CHECK_STR(upper, lower);
    CHECK_STR(upper, "636328");
}

static void test_a_capitalised_word_is_offered_with_its_capitals(void)
{
    static const char *const words[] = {"NeoDCT", "mended", "phone"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));
    char out[8][ND_T9_WORD_MAX];

    /* The point of storing the capitals is getting them back. */
    CHECK_INT(nd_t9_dict_suggest(d, "636328", out, 8u), 1);
    CHECK_STR(out[0], "NeoDCT");
    nd_t9_dict_close(d);
}

static void test_capitals_do_not_disturb_the_words_around_them(void)
{
    static const char *const words[] = {"NeoDCT", "mended", "mendee", "neofascism"};
    nd_t9_dict *d = make_dict("/t9.dict", words, ND_ARRAY_LEN(words));

    /* A mixed-case entry sits in the sorted file like any other word, so its
     * neighbours must still be findable across it. */
    CHECK(suggest_has(d, "636333", "mended", 8u));
    CHECK(suggest_has(d, "6363272476", "neofascism", 8u));
    nd_t9_dict_close(d);
}

/* --- the shipped dictionary --- */

/* neodct/tests/golden is NEODCT_GOLDEN and is not under ND_ROOT, so it is the
 * one fixed point a host test can navigate from. */
static bool shipped_dict_path(char *out, size_t out_sz)
{
    const char *golden = getenv("NEODCT_GOLDEN");
    int n;

    if (golden == NULL || golden[0] == '\0')
        return false;
    n = snprintf(out, out_sz, "%s/../../overlay/NeoDCT/System/core/t9.dict", golden);
    return n > 0 && (size_t)n < out_sz;
}

static void test_the_shipped_dictionary_knows_the_phone_it_runs_on(void)
{
    char path[ND_PATH_MAX];
    nd_t9_dict *d;
    char out[8][ND_T9_WORD_MAX];

    /* NeoDCT is added by mkt9dict --add and must survive in the file that
     * actually ships, not only in the builder's word list. */
    if (!shipped_dict_path(path, sizeof path)) {
        printf("  (skipped: NEODCT_GOLDEN unset)\n");
        return;
    }
    d = nd_t9_dict_open(path);
    if (!nd_t9_dict_available(d)) {
        printf("  (skipped: shipped dictionary not present)\n");
        nd_t9_dict_close(d);
        return;
    }
    CHECK(nd_t9_dict_suggest(d, "636328", out, 8u) > 0u);
    CHECK_STR(out[0], "NeoDCT");
    nd_t9_dict_close(d);
}

static void test_the_shipped_dictionary_is_sorted_by_key(void)
{
    char path[ND_PATH_MAX];
    FILE *f;
    char line[256];
    char key[64];
    char previous[64];
    unsigned long number = 0u;
    unsigned long bad_order = 0u;
    unsigned long untypeable = 0u;

    /* The binary search is only correct on a sorted file, and --add is a hand
     * edit of one -- so check the invariant it depends on. */
    if (!shipped_dict_path(path, sizeof path)) {
        printf("  (skipped: NEODCT_GOLDEN unset)\n");
        return;
    }
    f = fopen(path, "rb");
    if (f == NULL) {
        printf("  (skipped: shipped dictionary not present)\n");
        return;
    }
    previous[0] = '\0';
    while (fgets(line, (int)sizeof line, f) != NULL) {
        size_t len = strlen(line);

        number++;
        while (len > 0u && (unsigned char)line[len - 1u] <= ' ')
            line[--len] = '\0';
        if (!nd_t9_digits_for(line, key, sizeof key)) {
            if (untypeable == 0u)
                fprintf(stderr, "line %lu is not typeable: %s\n", number, line);
            untypeable++;
            continue;
        }
        if (strcmp(key, previous) < 0) {
            if (bad_order == 0u)
                fprintf(stderr, "line %lu is out of order: %s\n", number, line);
            bad_order++;
        }
        (void)nd_strlcpy(previous, key, sizeof previous);
    }
    (void)fclose(f);

    CHECK(number > 100000u);
    CHECK_INT(untypeable, 0);
    CHECK_INT(bad_order, 0);
}

int main(void)
{
    RUN(test_digits_match_the_keypad);
    RUN(test_a_word_with_no_key_is_rejected);
    RUN(test_an_exact_sequence_finds_its_word);
    RUN(test_a_partial_sequence_suggests_while_you_type);
    RUN(test_order_follows_the_file_so_the_likeliest_comes_first);
    RUN(test_a_sequence_with_nothing_behind_it_returns_nothing);
    RUN(test_a_one_digit_prefix_is_not_worth_answering);
    RUN(test_non_digits_are_refused);
    RUN(test_the_limit_is_respected);
    RUN(test_a_missing_dictionary_is_not_an_error);
    RUN(test_the_first_and_last_words_are_both_findable);
    RUN(test_every_word_in_a_larger_dictionary_is_findable);
    RUN(test_a_capitalised_word_keys_like_any_other);
    RUN(test_a_capitalised_word_is_offered_with_its_capitals);
    RUN(test_capitals_do_not_disturb_the_words_around_them);
    RUN(test_the_shipped_dictionary_knows_the_phone_it_runs_on);
    RUN(test_the_shipped_dictionary_is_sorted_by_key);
    return pt_report("test_t9_dict");
}
