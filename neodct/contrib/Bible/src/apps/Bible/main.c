/* apps/Bible/main.c -- the Bible, app id 13.
 *
 * The whole of scripture on a phone with 64 MB of RAM and a 240x175 screen,
 * in two registers: the World English Bible as its translators wrote it, and
 * the same text run through genz.c for anyone who was not going to read
 * Leviticus otherwise.
 *
 * ============ WHERE THE TEXT COMES FROM ============
 *
 * A .ndb pack -- format in bible.h, built by neodct/tools/mkbible.py. Packs
 * are looked for in two places, in this order:
 *
 *     /NeoDCT/User/Bible/[name].ndb    sideloaded, and wins
 *     <app dir>/[name].ndb            the shipped WEB pack, in the rootfs
 *
 * The user partition is searched FIRST and on purpose. "/" is a read-only
 * squashfs under dm-verity (AGENTS.md), so a translation somebody packed
 * themselves can only live on /NeoDCT/User -- and a pack a person went to the
 * trouble of copying onto the phone is more likely to be the one they want
 * open than the one that came with it. Both directories are listed together
 * in the Translation menu, so nothing becomes unreachable by being second.
 *
 * ============ THE STATE FILE IS NOT settings.prop ============
 *
 * Reading position, bookmarks and the mode live in
 * /NeoDCT/User/Bible/state.prop, written through nd_props_write_atomic().
 * settings.prop belongs to the core, which rewrites it whole from its own
 * merged view; an app setting a key in it races that rewrite for nothing,
 * since none of this is anybody else's business. It is written at the moment
 * it changes rather than on exit, because app_shutdown() is called from the
 * SIGTERM path and nd_app.h forbids it allocating -- and an incoming call
 * during Judges should not cost you your place.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_props.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "bibleapp.h"

#define BIBLE_TAG "bible"

#define BIBLE_MAX_PACKS 8

typedef struct {
    char path[ND_PATH_MAX];
    char label[48]; /* the file stem, which is what a person named it */
} bible_pack;

static bible_pack g_packs[BIBLE_MAX_PACKS];
static size_t g_n_packs;

/* Held so app_shutdown() can close the pack's descriptor. Nothing else is
 * global; see the teardown note at the bottom. */
static bible_app *g_app;

static const char *const ABOUT_TEXT =
    "NeoDCT Bible\n"
    "\n"
    "Text: the World English Bible, 2020 stable edition, from eBible.org. "
    "It is in the public domain -- not licensed, not permissioned, actually "
    "public domain -- which is the only reason a whole Bible can ship inside "
    "a phone image with no strings on it.\n"
    "\n"
    "81 books, including the deuterocanon, because if you are going to put "
    "the entire thing on a feature phone you may as well put the entire "
    "thing on it.\n"
    "\n"
    "GEN Z mode is a joke and is not a translation. It is a word "
    "substitution run over the WEB text at draw time; nothing is rewritten "
    "on disk. The WEB's terms ask that a changed text not be called the "
    "World English Bible any more, so while the mode is on the badge in the "
    "corner reads GEN Z and never WEB.\n"
    "\n"
    "Other translations: most modern ones are copyrighted and cannot be "
    "shipped here. neodct/tools/mkbible.py turns any verse-per-line export "
    "into a .ndb pack; put the result in /NeoDCT/User/Bible and it appears "
    "under Translation.\n"
    "\n"
    "Keys in the reader: up/down scroll, left/right change chapter, 0 pages "
    "down, 5 jumps to a verse, # is the next chapter, * the previous one, "
    "and the navi key opens Options.\n";

/* ------------------------------------------------------------------ *
 * References
 * ------------------------------------------------------------------ */

void bible_format_ref(const nd_bible *b, size_t book, size_t chapter, size_t verse, char *out,
                      size_t out_sz)
{
    const char *name = nd_bible_book_name(b, book);

    if (chapter == 0u)
        (void)nd_strlcpy(out, name, out_sz);
    else if (verse == 0u)
        (void)nd_snprintf(out, out_sz, "%s %u", name, (unsigned)chapter);
    else
        (void)nd_snprintf(out, out_sz, "%s %u:%u", name, (unsigned)chapter, (unsigned)verse);
}

static bool is_digit(char c)
{
    return c >= '0' && c <= '9';
}

/* "John 3:16" and "1 Cor 13" both have a number that belongs to the BOOK and
 * a number that belongs to the chapter, and only the leading position tells
 * them apart. So the name is taken to be everything up to the last space that
 * is not followed by a digit -- which is what makes "1 Cor" a name and "Cor
 * 13" a name plus a chapter. */
bool bible_parse_ref(const nd_bible *b, const char *s, size_t *book, size_t *chapter,
                     size_t *verse)
{
    char name[64];
    size_t name_len = 0u;
    const char *p = s;
    const char *num = NULL;
    int32_t idx;

    if (b == NULL || s == NULL || book == NULL || chapter == NULL || verse == NULL)
        return false;
    *chapter = 0u;
    *verse = 0u;

    while (*p == ' ')
        p++;
    /* A leading digit is part of the name ("1 Samuel"); every later run of
     * digits starts the chapter. */
    if (is_digit(*p)) {
        while (is_digit(*p) && name_len + 1u < sizeof name)
            name[name_len++] = *p++;
        while (*p == ' ' && name_len + 1u < sizeof name)
            name[name_len++] = *p++;
    }
    while (*p != '\0' && !is_digit(*p)) {
        if (name_len + 1u < sizeof name)
            name[name_len++] = *p;
        p++;
    }
    name[name_len] = '\0';
    /* Trim the space the name picked up before the chapter number. */
    while (name_len > 0u && name[name_len - 1u] == ' ')
        name[--name_len] = '\0';

    idx = nd_bible_find_book(b, name);
    if (idx < 0)
        return false;
    *book = (size_t)idx;

    if (*p == '\0')
        return true;
    num = p;
    *chapter = (size_t)strtoul(num, (char **)&p, 10);
    while (*p == ' ' || *p == ':' || *p == '.' || *p == ',')
        p++;
    if (is_digit(*p))
        *verse = (size_t)strtoul(p, NULL, 10);
    return true;
}

/* ------------------------------------------------------------------ *
 * State
 * ------------------------------------------------------------------ */

static void state_flush(bible_app *a)
{
    if (!a->state_dirty || a->state == NULL)
        return;
    if (nd_props_write_atomic(a->state_path, a->state, true) != ND_OK)
        nd_log(BIBLE_TAG, "could not write %s", a->state_path);
    a->state_dirty = false;
}

static void state_set_num(bible_app *a, const char *key, size_t value)
{
    char buf[24];

    (void)nd_snprintf(buf, sizeof buf, "%lu", (unsigned long)value);
    (void)nd_props_set(a->state, key, buf);
}

void bible_remember(bible_app *a, size_t book, size_t chapter, size_t verse)
{
    if (a->have_pos && a->book == book && a->chapter == chapter && a->verse == verse)
        return;
    a->book = book;
    a->chapter = chapter;
    a->verse = verse;
    a->have_pos = true;
    state_set_num(a, "book", book);
    state_set_num(a, "chapter", chapter);
    state_set_num(a, "verse", verse);
    (void)nd_props_set(a->state, "abbr", nd_bible_book_abbr(a->b, book));
    a->state_dirty = true;
    state_flush(a);
}

static void bookmark_key(size_t i, char *out, size_t out_sz)
{
    (void)nd_snprintf(out, out_sz, "mark.%02u", (unsigned)i);
}

/* A bookmark is stored as "<abbr> <chapter> <verse>" with the human's
 * 1-based chapter, so that a state file survives a pack whose book ORDER
 * differs -- the abbreviation is looked up again on the way back in. */
bool bible_bookmark_add(bible_app *a, size_t book, size_t chapter, size_t verse)
{
    char key[24];
    char value[64];
    size_t i;

    (void)nd_snprintf(value, sizeof value, "%s %u %u", nd_bible_book_abbr(a->b, book),
                      (unsigned)chapter + 1u, (unsigned)verse);
    for (i = 0u; i < BIBLE_MAX_BOOKMARKS; i++) {
        bookmark_key(i, key, sizeof key);
        if (!nd_props_has(a->state, key)) {
            if (nd_props_set(a->state, key, value) != ND_OK)
                return false;
            a->state_dirty = true;
            state_flush(a);
            return true;
        }
        if (strcmp(nd_props_get(a->state, key, ""), value) == 0)
            return true; /* already bookmarked; saying so twice is noise */
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * Finding packs
 * ------------------------------------------------------------------ */

static bool has_ndb_suffix(const char *name)
{
    size_t n = strlen(name);

    return n > 4u && strcmp(name + n - 4u, ".ndb") == 0;
}

static void scan_dir(const char *dir)
{
    char resolved[ND_PATH_MAX];
    DIR *d;
    struct dirent *ent;

    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return;
    d = opendir(resolved);
    if (d == NULL)
        return;
    while ((ent = readdir(d)) != NULL && g_n_packs < BIBLE_MAX_PACKS) {
        bible_pack *p;
        size_t stem;

        if (!has_ndb_suffix(ent->d_name))
            continue;
        p = &g_packs[g_n_packs];
        /* The UNRESOLVED path is stored: nd_bible_open() resolves it again,
         * and storing the resolved one would double the prefix under a test
         * root. */
        if (nd_snprintf(p->path, sizeof p->path, "%s/%s", dir, ent->d_name) != ND_OK)
            continue;
        (void)nd_strlcpy(p->label, ent->d_name, sizeof p->label);
        stem = strlen(p->label);
        if (stem > 4u)
            p->label[stem - 4u] = '\0'; /* drop ".ndb" */
        g_n_packs++;
    }
    (void)closedir(d);
}

static void find_packs(void)
{
    char app_dir[ND_PATH_MAX];

    g_n_packs = 0u;
    /* The user partition first; see the file header. */
    scan_dir(BIBLE_USER_DIR);
    if (nd_strlcpy(app_dir, nd_app_dir(), sizeof app_dir) < sizeof app_dir && app_dir[0] != '\0')
        scan_dir(app_dir);
}

/* ------------------------------------------------------------------ *
 * Screens
 * ------------------------------------------------------------------ */

static void show_note(nd_ui *ui, const char *message)
{
    nd_msgdialog dlg;

    nd_msgdialog_init(&dlg, ui, message);
    nd_msgdialog_set_button(&dlg, "OK");
    (void)nd_msgdialog_show(&dlg);
}

/* An "OK" softkey painted WITHOUT presenting, so that VerticalList's own
 * draw -- which clears rows 0..145 only -- leaves it standing. The same trick
 * Calculator uses; see its comment for why it is a throwaway bar. */
static void softkey_ok(nd_ui *ui)
{
    nd_softkey bar;

    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "OK", false);
}

static void chapter_then_read(bible_app *a, size_t book, size_t want_chapter, size_t want_verse)
{
    size_t n = nd_bible_chapter_count(a->b, book);
    int32_t chapter;

    if (n == 0u) {
        show_note(a->ui, "That book is empty in this pack.");
        return;
    }
    if (want_chapter >= 1u && want_chapter <= n) {
        bible_read(a, book, want_chapter - 1u, want_verse);
        return;
    }
    if (n == 1u) {
        bible_read(a, book, 0u, want_verse);
        return;
    }
    chapter = bible_pick_number(a->ui, nd_bible_book_name(a->b, book), 1, (int32_t)n, 1);
    if (chapter > 0)
        bible_read(a, book, (size_t)chapter - 1u, 0u);
}

/* One section's books. The index array is the caller's, so this allocates
 * nothing and the 81 name pointers come straight out of the pack's mapping. */
static void book_list(bible_app *a, const char *title, unsigned section)
{
    const char *names[128];
    size_t index[128];
    size_t n_books = nd_bible_book_count(a->b);
    size_t n = 0u;
    size_t i;

    for (i = 0u; i < n_books && n < 128u; i++) {
        if (nd_bible_book_section(a->b, i) != section)
            continue;
        names[n] = nd_bible_book_name(a->b, i);
        index[n] = i;
        n++;
    }
    if (n == 0u) {
        show_note(a->ui, "This pack has no books in that section.");
        return;
    }

    for (;;) {
        nd_vlist list;
        int32_t choice;

        softkey_ok(a->ui);
        nd_vlist_init(&list, a->ui, title, names, n, BIBLE_APP_ID);
        choice = nd_vlist_show(&list);
        if (choice < 0 || nd_app_should_exit())
            return;
        chapter_then_read(a, index[(size_t)choice], 0u, 0u);
    }
}

static void books_menu(bible_app *a)
{
    static const char *const SECTIONS[] = {"Old Testament", "New Testament", "Apocrypha"};
    static const unsigned SECTION_ID[] = {ND_NDB_OT, ND_NDB_NT, ND_NDB_APOCRYPHA};
    const char *items[3];
    unsigned ids[3];
    size_t n = 0u;
    size_t s;

    /* A pack with no deuterocanon must not show an empty Apocrypha row, and a
     * New-Testament-only pack should go straight to its books. */
    for (s = 0u; s < 3u; s++) {
        size_t i;

        for (i = 0u; i < nd_bible_book_count(a->b); i++) {
            if (nd_bible_book_section(a->b, i) == SECTION_ID[s]) {
                items[n] = SECTIONS[s];
                ids[n] = SECTION_ID[s];
                n++;
                break;
            }
        }
    }
    if (n == 0u) {
        show_note(a->ui, "This pack has no books.");
        return;
    }
    if (n == 1u) {
        book_list(a, items[0], ids[0]);
        return;
    }

    for (;;) {
        nd_vlist list;
        int32_t choice;

        softkey_ok(a->ui);
        nd_vlist_init(&list, a->ui, "Books", items, n, BIBLE_APP_ID);
        choice = nd_vlist_show(&list);
        if (choice < 0 || nd_app_should_exit())
            return;
        book_list(a, items[(size_t)choice], ids[(size_t)choice]);
    }
}

static void goto_screen(bible_app *a)
{
    char text[ND_TEXTINPUT_CAP];
    nd_textinput input;
    const char *answer;
    size_t book;
    size_t chapter;
    size_t verse;

    if (nd_textinput_init(&input, a->ui, "Go to", "e.g. John 3:16", text, sizeof text, "",
                          ND_T9_FILTER_ANY) != ND_OK)
        return;
    answer = nd_textinput_show(&input);
    if (answer == NULL || answer[0] == '\0')
        return;
    if (!bible_parse_ref(a->b, answer, &book, &chapter, &verse)) {
        show_note(a->ui, "No book by that name.");
        return;
    }
    chapter_then_read(a, book, chapter, verse);
}

/* ---- search ------------------------------------------------------ */

typedef struct {
    uint16_t book;
    uint16_t chapter;
    uint16_t verse;
} bible_hit;

typedef struct {
    bible_hit hits[BIBLE_MAX_HITS];
    char lines[BIBLE_MAX_HITS][160];
    const char *ptrs[BIBLE_MAX_HITS];
    size_t n;
} bible_results;

static bool contains_ci(const char *hay, const char *needle)
{
    size_t nlen = strlen(needle);
    const char *p;

    if (nlen == 0u)
        return false;
    for (p = hay; *p != '\0'; p++) {
        size_t i;

        for (i = 0u; i < nlen; i++) {
            char a = p[i];
            char b = needle[i];

            if (a >= 'A' && a <= 'Z')
                a = (char)(a + ('a' - 'A'));
            if (b >= 'A' && b <= 'Z')
                b = (char)(b + ('a' - 'A'));
            if (a != b)
                break;
        }
        if (i == nlen)
            return true;
    }
    return false;
}

/* Every chapter in the pack, inflated in turn. That is 1,402 inflates of a
 * couple of kilobytes each -- fast enough to be worth doing plainly, and the
 * alternative is a full-text index that would cost more on disk than the
 * text. Clear cancels: the scan polls between chapters rather than blocking,
 * which is the only reason it can be interrupted at all. */
static void search_scan(bible_app *a, const char *query, bible_results *res)
{
    nd_progress bar;
    size_t total = nd_bible_total_chapters(a->b);
    size_t done = 0u;
    size_t book;

    res->n = 0u;
    nd_progress_init(&bar, a->ui, "Searching", "Search", "Clear to stop", NULL, NULL);
    (void)nd_progress_draw(&bar, 0, (int64_t)total);

    for (book = 0u; book < nd_bible_book_count(a->b); book++) {
        size_t n_chapters = nd_bible_chapter_count(a->b, book);
        size_t chapter;

        for (chapter = 0u; chapter < n_chapters; chapter++) {
            size_t n_verses;
            size_t v;

            if (nd_bible_load(a->b, book, chapter) == ND_OK) {
                n_verses = nd_bible_verse_count(a->b);
                for (v = 1u; v <= n_verses && res->n < BIBLE_MAX_HITS; v++) {
                    const char *verse = nd_bible_verse(a->b, v);
                    char ref[48];

                    if (!contains_ci(verse, query))
                        continue;
                    res->hits[res->n].book = (uint16_t)book;
                    res->hits[res->n].chapter = (uint16_t)chapter;
                    res->hits[res->n].verse = (uint16_t)v;
                    bible_format_ref(a->b, book, chapter + 1u, v, ref, sizeof ref);
                    (void)nd_snprintf(res->lines[res->n], sizeof res->lines[0], "%s  %s", ref,
                                      verse);
                    res->ptrs[res->n] = res->lines[res->n];
                    res->n++;
                }
            }
            done++;
            /* Repainting per chapter would cost more than the search; the
             * widget itself declines to redraw when the percentage has not
             * moved, so this is one call and usually no pixels. */
            (void)nd_progress_draw(&bar, (int64_t)done, (int64_t)total);

            if (res->n >= BIBLE_MAX_HITS)
                return;
            if (nd_app_should_exit())
                return;
            if (nd_ui_read_keypress(a->ui, 0.0) == ND_KEY_CLEAR)
                return;
        }
    }
}

static void search_screen(bible_app *a)
{
    char query[ND_TEXTINPUT_CAP];
    nd_textinput input;
    const char *answer;
    bible_results *res;

    if (nd_textinput_init(&input, a->ui, "Search", "word or phrase", query, sizeof query, "",
                          ND_T9_FILTER_ANY) != ND_OK)
        return;
    answer = nd_textinput_show(&input);
    if (answer == NULL || answer[0] == '\0')
        return;

    /* 10 KB of hit text. On the heap because CODING-STANDARDS.md section 1.5
     * keeps anything this size off the stack even when its bound is fixed. */
    res = calloc(1u, sizeof *res);
    if (res == NULL) {
        show_note(a->ui, "Not enough memory to search.");
        return;
    }
    search_scan(a, answer, res);

    if (res->n == 0u) {
        show_note(a->ui, "Nothing found.");
        free(res);
        return;
    }
    for (;;) {
        nd_pagedlist page;
        char title[64];
        int32_t choice;

        (void)nd_snprintf(title, sizeof title, "%u found%s", (unsigned)res->n,
                          (res->n >= BIBLE_MAX_HITS) ? "+" : "");
        nd_pagedlist_init(&page, a->ui, title, res->ptrs, res->n, "13", true);
        choice = nd_pagedlist_show(&page);
        if (choice < 0 || nd_app_should_exit())
            break;
        bible_read(a, res->hits[choice].book, res->hits[choice].chapter,
                   res->hits[choice].verse);
    }
    free(res);
}

/* ---- bookmarks --------------------------------------------------- */

static void bookmarks_screen(bible_app *a)
{
    char labels[BIBLE_MAX_BOOKMARKS][64];
    const char *ptrs[BIBLE_MAX_BOOKMARKS];
    bible_hit hits[BIBLE_MAX_BOOKMARKS];
    size_t n = 0u;
    size_t i;

    for (i = 0u; i < BIBLE_MAX_BOOKMARKS; i++) {
        char key[24];
        const char *raw;
        char abbr[8];
        unsigned chapter = 0u;
        unsigned verse = 0u;
        int32_t book;

        bookmark_key(i, key, sizeof key);
        raw = nd_props_get(a->state, key, NULL);
        if (raw == NULL)
            continue;
        if (sscanf(raw, "%7s %u %u", abbr, &chapter, &verse) != 3)
            continue;
        book = nd_bible_find_book(a->b, abbr);
        if (book < 0 || chapter == 0u)
            continue; /* a bookmark from a pack this one does not contain */
        hits[n].book = (uint16_t)book;
        hits[n].chapter = (uint16_t)(chapter - 1u);
        hits[n].verse = (uint16_t)verse;
        bible_format_ref(a->b, (size_t)book, chapter, verse, labels[n], sizeof labels[0]);
        ptrs[n] = labels[n];
        n++;
    }
    if (n == 0u) {
        show_note(a->ui, "No bookmarks yet. Add one from Options while reading.");
        return;
    }

    for (;;) {
        nd_vlist list;
        int32_t choice;

        softkey_ok(a->ui);
        nd_vlist_init(&list, a->ui, "Bookmarks", ptrs, n, BIBLE_APP_ID);
        choice = nd_vlist_show(&list);
        if (choice < 0 || nd_app_should_exit())
            return;
        bible_read(a, hits[choice].book, hits[choice].chapter, hits[choice].verse);
    }
}

/* ---- random ------------------------------------------------------ */

/* Uniform over CHAPTERS rather than over books, so that Psalms is a hundred
 * and fifty times as likely as Obadiah -- which is what "a random verse"
 * means to anyone who is not counting books. */
static void random_verse(bible_app *a)
{
    size_t total = nd_bible_total_chapters(a->b);
    size_t target;
    size_t book;
    size_t seen = 0u;

    if (total == 0u)
        return;
    nd_rand_seed((uint32_t)nd_time_now());
    target = (size_t)nd_rand_below((int32_t)total);

    for (book = 0u; book < nd_bible_book_count(a->b); book++) {
        size_t n = nd_bible_chapter_count(a->b, book);

        if (target < seen + n) {
            size_t chapter = target - seen;
            size_t verse = 1u;

            if (nd_bible_load(a->b, book, chapter) == ND_OK &&
                nd_bible_verse_count(a->b) > 0u)
                verse = (size_t)nd_rand_below((int32_t)nd_bible_verse_count(a->b)) + 1u;
            bible_read(a, book, chapter, verse);
            return;
        }
        seen += n;
    }
}

/* ---- translation ------------------------------------------------- */

static nd_err open_pack(bible_app *a, const char *path)
{
    nd_bible *opened = NULL;
    nd_err rc = nd_bible_open(&opened, path);

    if (rc != ND_OK)
        return rc;
    nd_bible_close(a->b);
    a->b = opened;
    /* The old pack's indices mean nothing in the new one. The bookmark list
     * survives because it stores abbreviations, not indices. */
    a->have_pos = false;
    (void)nd_props_set(a->state, "pack", path);
    a->state_dirty = true;
    state_flush(a);
    return ND_OK;
}

static void translation_screen(bible_app *a)
{
    const char *items[BIBLE_MAX_PACKS];
    size_t i;
    nd_vlist list;
    int32_t choice;

    for (i = 0u; i < g_n_packs; i++)
        items[i] = g_packs[i].label;

    softkey_ok(a->ui);
    nd_vlist_init(&list, a->ui, "Translation", items, g_n_packs, BIBLE_APP_ID);
    choice = nd_vlist_show(&list);
    if (choice < 0)
        return;
    if (open_pack(a, g_packs[choice].path) != ND_OK)
        show_note(a->ui, "That pack could not be opened.");
}

/* ------------------------------------------------------------------ *
 * The main menu
 * ------------------------------------------------------------------ */

typedef enum {
    M_CONTINUE = 0,
    M_BOOKS,
    M_GOTO,
    M_SEARCH,
    M_RANDOM,
    M_BOOKMARKS,
    M_STYLE,
    M_TRANSLATION,
    M_ABOUT,
    M_COUNT
} menu_id;

static void main_menu(bible_app *a)
{
    for (;;) {
        const char *items[M_COUNT];
        menu_id ids[M_COUNT];
        char continue_label[64];
        char style_label[48];
        size_t n = 0u;
        nd_vlist list;
        int32_t choice;

        if (a->have_pos) {
            char ref[48];

            bible_format_ref(a->b, a->book, a->chapter + 1u, 0u, ref, sizeof ref);
            (void)nd_snprintf(continue_label, sizeof continue_label, "Continue: %s", ref);
            items[n] = continue_label;
            ids[n++] = M_CONTINUE;
        }
        items[n] = "Books";
        ids[n++] = M_BOOKS;
        items[n] = "Go to...";
        ids[n++] = M_GOTO;
        items[n] = "Search";
        ids[n++] = M_SEARCH;
        items[n] = "Random verse";
        ids[n++] = M_RANDOM;
        items[n] = "Bookmarks";
        ids[n++] = M_BOOKMARKS;

        (void)nd_snprintf(style_label, sizeof style_label, "Style: %s",
                          a->genz ? "GEN Z" : nd_bible_translation(a->b));
        items[n] = style_label;
        ids[n++] = M_STYLE;

        if (g_n_packs > 1u) {
            items[n] = "Translation";
            ids[n++] = M_TRANSLATION;
        }
        items[n] = "About";
        ids[n++] = M_ABOUT;

        softkey_ok(a->ui);
        nd_vlist_init(&list, a->ui, "Bible", items, n, BIBLE_APP_ID);
        choice = nd_vlist_show(&list);
        if (choice < 0 || nd_app_should_exit())
            return;

        switch (ids[(size_t)choice]) {
        case M_CONTINUE:
            bible_read(a, a->book, a->chapter, a->verse);
            break;
        case M_BOOKS:
            books_menu(a);
            break;
        case M_GOTO:
            goto_screen(a);
            break;
        case M_SEARCH:
            search_screen(a);
            break;
        case M_RANDOM:
            random_verse(a);
            break;
        case M_BOOKMARKS:
            bookmarks_screen(a);
            break;
        case M_STYLE:
            a->genz = !a->genz;
            (void)nd_props_set(a->state, "genz", a->genz ? "1" : "0");
            a->state_dirty = true;
            state_flush(a);
            break;
        case M_TRANSLATION:
            translation_screen(a);
            break;
        case M_ABOUT: {
            nd_scroller s;

            nd_scroller_init(&s, a->ui, ABOUT_TEXT, "More", "Back");
            nd_scroller_show(&s);
            break;
        }
        default:
            break;
        }
    }
}

/* ------------------------------------------------------------------ *
 * Start-up
 * ------------------------------------------------------------------ */

static void load_state(bible_app *a)
{
    const char *saved;

    (void)nd_mkdir_p(BIBLE_USER_DIR, 0755u);
    (void)nd_snprintf(a->state_path, sizeof a->state_path, "%s%s", BIBLE_USER_DIR,
                      BIBLE_STATE_REL);
    if (nd_props_parse_raw(a->state_path, &a->state) != ND_OK || a->state == NULL)
        a->state = nd_props_new();
    if (a->state == NULL)
        return;

    a->genz = strcmp(nd_props_get(a->state, "genz", "0"), "1") == 0;

    saved = nd_props_get(a->state, "abbr", NULL);
    if (saved != NULL) {
        int32_t book = nd_bible_find_book(a->b, saved);

        if (book >= 0) {
            a->book = (size_t)book;
            a->chapter = (size_t)strtoul(nd_props_get(a->state, "chapter", "0"), NULL, 10);
            a->verse = (size_t)strtoul(nd_props_get(a->state, "verse", "1"), NULL, 10);
            if (a->chapter < nd_bible_chapter_count(a->b, a->book))
                a->have_pos = true;
        }
    }
}

/* The pack the state file names, if it is still there; otherwise the first
 * one found, which puts the user partition ahead of the rootfs. */
static nd_err open_preferred_pack(bible_app *a, nd_props *early)
{
    const char *wanted = (early != NULL) ? nd_props_get(early, "pack", NULL) : NULL;
    size_t i;

    if (wanted != NULL) {
        for (i = 0u; i < g_n_packs; i++) {
            if (strcmp(g_packs[i].path, wanted) == 0 &&
                nd_bible_open(&a->b, g_packs[i].path) == ND_OK)
                return ND_OK;
        }
    }
    for (i = 0u; i < g_n_packs; i++) {
        if (nd_bible_open(&a->b, g_packs[i].path) == ND_OK)
            return ND_OK;
    }
    return ND_ERR_NOTFOUND;
}

int app_run(nd_ui *ui)
{
    static bible_app app;
    nd_props *early = NULL;
    char state_path[ND_PATH_MAX];

    if (ui == NULL)
        return 1;

    memset(&app, 0, sizeof app);
    app.ui = ui;
    g_app = &app;

    find_packs();
    if (g_n_packs == 0u) {
        show_note(ui,
                  "No Bible pack found. Put a .ndb file in /NeoDCT/User/Bible "
                  "-- see tools/mkbible.py.");
        return 0;
    }

    /* The state file is read twice on purpose: once here, only for the "which
     * pack" key, because which pack is open decides how every other key in it
     * is interpreted, and once properly in load_state() afterwards. */
    (void)nd_snprintf(state_path, sizeof state_path, "%s%s", BIBLE_USER_DIR, BIBLE_STATE_REL);
    (void)nd_props_parse_raw(state_path, &early);
    if (open_preferred_pack(&app, early) != ND_OK) {
        nd_props_free(early);
        show_note(ui, "The Bible pack is damaged and could not be opened.");
        return 0;
    }
    nd_props_free(early);

    load_state(&app);
    if (app.state == NULL) {
        nd_bible_close(app.b);
        app.b = NULL;
        return 1;
    }

    main_menu(&app);

    state_flush(&app);
    nd_props_free(app.state);
    app.state = NULL;
    nd_bible_close(app.b);
    app.b = NULL;
    g_app = NULL;
    return 0;
}

/* One open file descriptor and two heap buffers. Closing the pack is cheap
 * and unblocking, which is what nd_app.h asks for; the state file is NOT
 * written here, because nd_props_write_atomic() allocates and this runs from
 * the SIGTERM path. Every change that mattered was flushed when it happened.
 */
void app_shutdown(void)
{
    if (g_app != NULL) {
        nd_bible_close(g_app->b);
        g_app->b = NULL;
        g_app = NULL;
    }
}
