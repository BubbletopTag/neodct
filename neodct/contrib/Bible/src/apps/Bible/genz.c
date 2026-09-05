/* genz.c -- the other translation.
 *
 * A display filter. It never writes back into the pack, and the reader keeps
 * the label on screen as "GEN Z" while it is on, because the World English
 * Bible's terms ask exactly one thing of anyone who changes the text: stop
 * calling the result the World English Bible. Two lines of code and a label
 * honour that completely, so there was no reason not to.
 *
 * ============ WHAT IT DOES NOT TOUCH ============
 *
 * Yahweh, God, Jesus, Christ, Lord and Spirit are not in the table and will
 * not be. The joke here is the register -- a text that has survived three
 * thousand years being read out by somebody who has never finished a book --
 * and it stops being that the moment it starts being a swipe at what the text
 * is about. Proper names generally are left alone for the duller reason that
 * a substitution table cannot tell Peter the apostle from a peter of any
 * other kind.
 *
 * ============ WHY THE OUTPUT IS DETERMINISTIC ============
 *
 * The interjection at the front and the tag at the end are picked by `seed`,
 * which the caller derives from the verse's own (book, chapter, verse). A
 * rand() would have been easier and would have reshuffled the text under a
 * scrolling reader: page down and back up and the verse would read
 * differently, which looks like a rendering fault rather than a joke. Same
 * verse, same words, every time.
 *
 * ============ WHY THE TABLE IS ORDERED BY LENGTH ============
 *
 * Matching is first-match-wins at each word boundary, so "said to them" has
 * to be tried before "said" or it can never fire. The table below is
 * hand-ordered longest-first and test_bible.c asserts that it still is,
 * because the failure mode of getting it wrong is not a crash -- it is one
 * phrase quietly never appearing again.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "bible.h"

/* ------------------------------------------------------------------ *
 * The lexicon
 * ------------------------------------------------------------------ */

/* Ordered strictly longest `from` first. See the header comment. */
static const nd_genz_pair GENZ[] = {
    {"children of Israel", "Israel gang"},
    {"it came to pass", "so anyway"},
    {"said to them", "hit up the squad like"},
    {"said to him", "hit up bro like"},
    {"said to her", "hit up shorty like"},
    {"it happened", "so anyway"},
    {"exceedingly", "hella"},
    {"abomination", "absolute ick"},
    {"inheritance", "the bag"},
    {"righteous", "based"},
    {"therefore", "so basically"},
    {"multitude", "whole crowd"},
    {"commanded", "ordered"},
    {"beautiful", "fire"},
    {"marvelous", "insane"},
    {"wonderful", "insane"},
    {"come from", "straight outta"},
    {"answered", "clapped back"},
    {"garments", "fits"},
    {"covenant", "the deal"},
    {"servants", "interns"},
    {"strength", "aura"},
    {"garment", "fit"},
    {"servant", "intern"},
    {"enemies", "ops"},
    {"foolish", "npc"},
    {"blessed", "goated"},
    {"rejoice", "vibe"},
    {"riches", "bands"},
    {"behold", "yo peep this"},
    {"wicked", "sus"},
    {"wisdom", "big brain"},
    {"afraid", "shook"},
    {"battle", "the beef"},
    {"people", "homies"},
    {"perish", "get cooked"},
    {"praise", "hype"},
    {"surely", "fr"},
    {"indeed", "fr"},
    {"spoke", "yapped"},
    {"glory", "clout"},
    {"enemy", "op"},
    {"truly", "no cap"},
    {"money", "bands"},
    {"fools", "npcs"},
    {"women", "queens"},
    {"great", "massive"},
    {"woman", "queen"},
    {"truth", "facts"},
    {"anger", "big mad energy"},
    {"wrath", "big mad"},
    {"peace", "good vibes"},
    {"sword", "blade"},
    {"shall", "gonna"},
    {"came", "pulled up"},
    {"come", "pull up"},
    {"went", "rolled out"},
    {"made", "cooked up"},
    {"land", "turf"},
    {"gold", "bling"},
    {"fool", "npc"},
    {"true", "facts"},
    {"evil", "cursed"},
    {"very", "hella"},
    {"king", "top G"},
    {"said", "was like"},
    {"good", "bussin"},
    {"sins", "Ls"},
    {"sin", "an L"},
    {"war", "beef"},
    {"men", "bros"},
    {"man", "bro"},
};

#define GENZ_N (sizeof GENZ / sizeof GENZ[0])

const nd_genz_pair *nd_genz_table(size_t *n_entries)
{
    if (n_entries != NULL)
        *n_entries = GENZ_N;
    return GENZ;
}

/* Not every verse gets one. The empty strings are entries on purpose: they
 * are how roughly a third of verses come out unadorned, which is what stops
 * the whole book reading like one sustained shout. */
static const char *const PREFIX[] = {"",         "ok so ",          "yo, ",  "",
                                     "listen, ", "not gonna lie, ", "",      "bruh, "};
#define PREFIX_N (sizeof PREFIX / sizeof PREFIX[0])

static const char *const SUFFIX[] = {"", " no cap.", "", " fr.", "",
                                     " ong.", "", " deadass."};
#define SUFFIX_N (sizeof SUFFIX / sizeof SUFFIX[0])

/* ------------------------------------------------------------------ *
 * The seed
 * ------------------------------------------------------------------ */

/* FNV-1a over the three numbers. Any mixing function would do; this one is
 * four lines and has no table. */
uint32_t nd_genz_seed(size_t book, size_t chapter, size_t verse)
{
    uint32_t h = 2166136261u;
    uint32_t parts[3];
    size_t i;

    parts[0] = (uint32_t)book;
    parts[1] = (uint32_t)chapter;
    parts[2] = (uint32_t)verse;
    for (i = 0u; i < 3u; i++) {
        unsigned b;

        for (b = 0u; b < 4u; b++) {
            h ^= (parts[i] >> (b * 8u)) & 0xFFu;
            h *= 16777619u;
        }
    }
    return h;
}

/* ------------------------------------------------------------------ *
 * Matching
 * ------------------------------------------------------------------ */

static bool is_alpha(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + ('a' - 'A')) : c;
}

static char upper(char c)
{
    return (c >= 'a' && c <= 'z') ? (char)(c - ('a' - 'A')) : c;
}

/* Whole-word, case-insensitive. The end boundary allows an apostrophe so
 * "man's" matches "man" and comes out "bro's", which is the common case in
 * this text and the reason the boundary is not simply !is_alpha. */
static bool word_match(const char *s, const char *word, size_t wlen)
{
    size_t i;

    for (i = 0u; i < wlen; i++) {
        if (lower(s[i]) != lower(word[i]))
            return false;
    }
    return !is_alpha(s[wlen]);
}

/* ------------------------------------------------------------------ *
 * A bounded appender
 * ------------------------------------------------------------------ */

/* snprintf semantics without the formatting: *len tracks what the caller
 * WANTED to write, so it keeps counting past the end of the buffer and the
 * final length is a truncation test rather than a lie. */
static void put(char *out, size_t out_sz, size_t *len, const char *s, size_t n)
{
    size_t i;

    for (i = 0u; i < n; i++) {
        if (*len + 1u < out_sz)
            out[*len] = s[i];
        (*len)++;
    }
}

/* ------------------------------------------------------------------ *
 * nd_genz
 * ------------------------------------------------------------------ */

size_t nd_genz(char *out, size_t out_sz, const char *in, uint32_t seed)
{
    const char *p;
    size_t len = 0u;
    bool at_word_start = true;

    if (out == NULL || out_sz == 0u || in == NULL)
        return 0u;
    if (in[0] == '\0') {
        out[0] = '\0';
        return 0u;
    }

    {
        const char *pre = PREFIX[seed % PREFIX_N];

        put(out, out_sz, &len, pre, strlen(pre));
    }

    for (p = in; *p != '\0';) {
        size_t i;
        bool hit = false;

        if (!at_word_start || !is_alpha(*p)) {
            at_word_start = !is_alpha(*p);
            put(out, out_sz, &len, p, 1u);
            p++;
            continue;
        }

        for (i = 0u; i < GENZ_N; i++) {
            size_t wlen = strlen(GENZ[i].from);
            const char *to;
            size_t tlen;

            if (!word_match(p, GENZ[i].from, wlen))
                continue;

            to = GENZ[i].to;
            tlen = strlen(to);
            /* Carry the source's capitalisation onto the replacement, so a
             * verse that opens with "Behold," opens with "Yo peep this,". */
            if (upper(*p) == *p && tlen > 0u) {
                char c = upper(to[0]);

                put(out, out_sz, &len, &c, 1u);
                put(out, out_sz, &len, to + 1, tlen - 1u);
            } else {
                put(out, out_sz, &len, to, tlen);
            }
            p += wlen;
            at_word_start = false;
            hit = true;
            break;
        }
        if (hit)
            continue;

        /* No rule fired: copy the whole word so that its interior can never
         * be re-examined as if it started a word. */
        while (is_alpha(*p)) {
            put(out, out_sz, &len, p, 1u);
            p++;
        }
        at_word_start = false;
    }

    {
        const char *suf = SUFFIX[(seed / PREFIX_N) % SUFFIX_N];

        put(out, out_sz, &len, suf, strlen(suf));
    }

    out[(len < out_sz) ? len : out_sz - 1u] = '\0';
    return len;
}
