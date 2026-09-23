/* test_themepicker.c -- the paged theme chooser.
 *
 * The picker's own loop cannot be driven from here: nd_detailpage_show()
 * blocks on the key channel, and smallapp_test.h's note about a blocking
 * widget draining the channel before its first draw applies to it exactly.
 * So what is checked is everything up to the loop -- which theme the picker
 * opens on, what the list contains, and that a page for each theme actually
 * lays out with that theme applied.
 *
 * That last one is the claim worth defending. The picker previews a theme by
 * WEARING it: it applies the palette and then measures and draws the page. If
 * a theme could be applied but not laid out -- an empty name, a description
 * longer than the block array -- the preview would be a blank screen with no
 * way back, and it would happen on the owner's phone rather than here.
 */

#include <string.h>

#include "nd_theme.h"
#include "nd_widgets.h"
#include "smallapp_test.h"

#define TP_MAX (ND_PATH_MAX * 2)

/* smallapp_test.h's RUN() does not hand each case a fresh root the way
 * platform_test.h's does, so a theme written by one case is still on disk for
 * the next -- which showed up as "three of them: got 5". Each case clears the
 * directory itself. */
static void reset_themes(void)
{
    char real[TP_MAX];

    if (nd_path_resolve(real, sizeof real, ND_PATH_THEMES_DIR) == ND_OK)
        sa_rmtree(real);
    (void)nd_mkdir_p(ND_PATH_THEMES_DIR, 0755u);
}

/* Finds a theme by id. Indexing by position is what tied the first draft of
 * this test to the alphabet. */
static size_t at(const nd_themepicker *p, const char *id)
{
    size_t i;

    for (i = 0u; i < p->n; i++) {
        if (strcmp(p->themes[i].id, id) == 0)
            return i;
    }
    return (size_t)-1;
}

static void mk(const char *name, const char *json)
{
    char dir[TP_MAX];
    char path[TP_MAX + 32];
    char real[TP_MAX + 64];
    FILE *f;

    (void)nd_mkdir_p(ND_PATH_THEMES_DIR, 0755u);
    (void)snprintf(dir, sizeof dir, "%s/%s", ND_PATH_THEMES_DIR, name);
    (void)nd_mkdir_p(dir, 0755u);
    (void)snprintf(path, sizeof path, "%s/theme.json", dir);
    if (nd_path_resolve(real, sizeof real, path) != ND_OK)
        return;
    f = fopen(real, "w");
    if (f != NULL) {
        (void)fputs(json, f);
        (void)fclose(f);
    }
}

/* The built-in is offered first, and the picker opens on whatever is active
 * rather than at the top -- an owner arriving at this screen is looking at
 * the theme they are wearing. */
static void test_it_opens_on_the_active_theme(void)
{
    sa_fixture fx;
    nd_themepicker picker;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    reset_themes();

    mk("Rose", "{\"id\":\"rose\",\"name\":\"Rose\"}");
    mk("Mint", "{\"id\":\"mint\",\"name\":\"Mint\"}");

    CHECK_INT(nd_themepicker_init(&picker, &fx.ui), ND_OK, "the picker opens");
    CHECK_INT(picker.n, 3, "the built-in and the two on disk");
    CHECK_STR(picker.themes[0].id, ND_THEME_ID_BUILTIN, "the built-in is first");
    CHECK_INT(picker.sel, 0, "and is where the selection starts, being active");
    /* Sorted by name within the group after the built-in. */
    CHECK_STR(picker.themes[1].name, "Mint", "then the disk themes, by name");
    CHECK_STR(picker.themes[2].name, "Rose", "M before R");

    CHECK_INT(nd_theme_select("rose"), ND_OK, "wear one");
    CHECK_INT(nd_themepicker_init(&picker, &fx.ui), ND_OK, "and open the picker again");
    CHECK_STR(picker.themes[picker.sel].id, "rose", "it opens on the one being worn");
    CHECK_STR(picker.entry_id, "rose", "and remembers it, to put back on a cancel");

    nd_theme_apply(NULL);
    sa_fx_free(&fx);
}

/* Every theme in the list must survive being worn and laid out. A theme whose
 * page cannot be built is a preview the owner cannot get out of. */
static void test_every_theme_lays_a_page_out(void)
{
    sa_fixture fx;
    nd_themepicker picker;
    size_t i;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    reset_themes();

    mk("Bare", "{\"id\":\"bare\"}"); /* no name, no description, no author */
    mk("Wordy", "{\"id\":\"wordy\",\"name\":\"Wordy\",\"description\":"
                "\"A description long enough to wrap several times over, which is what "
                "the scrolling half of the page is for, and which is exactly the shape "
                "that would overflow a block array if one were sized by guesswork "
                "rather than measured against the text it has to hold.\"}");

    CHECK_INT(nd_themepicker_init(&picker, &fx.ui), ND_OK, "the picker opens");
    CHECK_INT(picker.n, 3, "three of them");

    for (i = 0u; i < picker.n; i++) {
        nd_detailpage page;
        char badge[16];

        /* What the picker does on every keypress: wear it, then lay out. */
        nd_theme_apply(picker.themes[i].builtin ? NULL : &picker.themes[i]);
        (void)nd_snprintf(badge, sizeof badge, "%zu/%zu", i + 1u, picker.n);

        CHECK_INT(nd_detailpage_init(&page, &fx.ui, picker.themes[i].name, NULL,
                                     picker.themes[i].desc, NULL, badge, "Theme", "Apply"),
                  ND_OK, "the page lays out");
        CHECK(page.n_blocks > 0u, "and has something in it");
        nd_detailpage_draw(&page); /* must not fault */
        nd_detailpage_free(&page);
    }

    /* A theme with no name of its own shows its id rather than an empty
     * title bar. */
    CHECK_STR(picker.themes[at(&picker, "bare")].name, "bare",
              "the nameless one is titled with its id");

    nd_theme_apply(NULL);
    sa_fx_free(&fx);
}

/* Wearing a theme to preview it must not leak past the picker: whatever was
 * on when the owner walked in is what they walk out with unless they choose. */
static void test_the_palette_is_restored_when_nothing_is_chosen(void)
{
    sa_fixture fx;
    nd_themepicker picker;
    nd_color before;

    if (!sa_fx_init(&fx)) {
        CHECK(false, "fixture");
        sa_fx_free(&fx);
        return;
    }
    reset_themes();

    mk("Loud", "{\"id\":\"loud\",\"palette\":{\"blue_top\":\"#FF0000\"}}");

    nd_theme_apply(NULL);
    before = ND_TH_BLUE_TOP;
    CHECK_INT(nd_themepicker_init(&picker, &fx.ui), ND_OK, "the picker opens");

    /* Hover it, as moving onto its page would. */
    CHECK(at(&picker, "loud") != (size_t)-1, "the loud one is in the list");
    nd_theme_apply(&picker.themes[at(&picker, "loud")]);
    CHECK_INT(ND_TH_BLUE_TOP.r, 0xFF, "the preview is really applied");

    /* nd_themepicker_show() ends by restoring entry_id; do what it does. */
    {
        nd_theme_info back;

        if (strcmp(picker.entry_id, ND_THEME_ID_BUILTIN) == 0 ||
            !nd_theme_find(picker.entry_id, &back))
            nd_theme_apply(NULL);
        else
            nd_theme_apply(&back);
    }
    CHECK_INT(ND_TH_BLUE_TOP.r, before.r, "and put back when nothing is chosen");
    CHECK_STR(nd_theme_active()->id, ND_THEME_ID_BUILTIN, "back to what was worn");

    sa_fx_free(&fx);
}

int main(void)
{
    void *h = sa_begin("Settings", "ndthemepick");
    int rc;

    if (h == NULL)
        return 1;

    RUN(test_it_opens_on_the_active_theme);
    RUN(test_every_theme_lays_a_page_out);
    RUN(test_the_palette_is_restored_when_nothing_is_chosen);

    nd_theme_apply(NULL);
    rc = sa_end(h, "test_themepicker");
    return rc;
}
