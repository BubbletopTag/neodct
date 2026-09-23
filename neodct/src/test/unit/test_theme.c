/* test_theme.c -- the theme loader: what a theme file is allowed to be, and
 * what happens when it is not that.
 *
 * The palette itself is covered by the GOLDEN FRAMES rather than by anything
 * here, and deliberately: the claim worth defending is "the built-in defaults
 * render the pixels they always did", and only a rendered frame can say that.
 * What this file covers is the part a frame cannot see -- parsing, the
 * registry's precedence, the fallbacks, and the resource mapping.
 */

#include <string.h>

#include "nd_settings.h"
#include "nd_theme.h"
#include "platform_test.h"

/* A theme directory under the case root, with whatever theme.json it is
 * given. Returns the VIRTUAL path, which is what every entry point takes. */
/* Roomier than ND_PATH_MAX on purpose. -Wformat-truncation is an error here
 * and the compiler cannot prove that "<dir>/theme.json" fits a buffer the
 * same size as <dir>; the fixture's names are short and a scratch root is
 * short, so the honest fix is a destination that provably cannot overflow
 * rather than a check the test would never take. */
#define TP_MAX (ND_PATH_MAX * 2)

static const char *make_theme(const char *dir_name, const char *json)
{
    static char dir[TP_MAX];
    char path[TP_MAX + 32];

    pt_mkdir(ND_PATH_THEMES_DIR);
    (void)snprintf(dir, sizeof dir, "%s/%s", ND_PATH_THEMES_DIR, dir_name);
    pt_mkdir(dir);
    (void)snprintf(path, sizeof path, "%s/theme.json", dir);
    pt_write_text(path, json);
    return dir;
}

static void touch(const char *dir, const char *rel)
{
    char path[TP_MAX * 2];
    const char *slash = strrchr(rel, '/');

    if (slash != NULL) {
        char sub[TP_MAX * 2];

        (void)snprintf(sub, sizeof sub, "%s/%.*s", dir, (int)(slash - rel), rel);
        pt_mkdir(sub);
    }
    (void)snprintf(path, sizeof path, "%s/%s", dir, rel);
    pt_write_text(path, "x");
}

/* ------------------------------------------------------------------ */

/* The defaults are the compiled-in look, and nothing has replaced them before
 * a theme is loaded. This is the invariant the golden frames rest on. */
static void test_the_built_in_is_active_before_anything_is_loaded(void)
{
    const nd_theme_palette *p = nd_theme_pal;

    CHECK(p == nd_theme_palette_builtin());
    /* The built-in is the CLASSIC look: white type on black, one ink, no
     * gloss. Asserted by its two most characteristic values rather than by
     * every field -- the signature colour is white because the selection is
     * an inverted row, and the background is black. */
    CHECK_INT(p->blue_top.r, 0xFF);
    CHECK_INT(p->blue_top.g, 0xFF);
    CHECK_INT(p->blue_top.b, 0xFF);
    CHECK_INT(p->sky_top.r, 0x00);
    CHECK_INT(p->sky_bot.b, 0x00);
    CHECK_INT(p->ink_light.r, 0xFF);
    CHECK(!nd_theme_style_of->gloss);
    CHECK(!nd_theme_style_of->round);
    CHECK(nd_theme_style_of->pixel_font);
    CHECK_STR(nd_theme_active()->id, ND_THEME_ID_BUILTIN);
}

/* A theme names three colours; the other twenty-one keep the built-in's. That
 * is what makes a recolour nine lines rather than a copy of the whole
 * palette, and it is the behaviour a theme author will assume. */
static void test_an_absent_colour_keeps_the_built_in_value(void)
{
    nd_theme_info t;
    const char *dir = make_theme("Partial",
                                 "{\"id\":\"partial\",\"name\":\"Partial\","
                                 "\"palette\":{\"blue_top\":\"#FF0000\"}}");

    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    CHECK_STR(t.id, "partial");
    CHECK_INT(t.palette.blue_top.r, 0xFF);
    CHECK_INT(t.palette.blue_top.g, 0x00);
    /* Untouched -- and compared against the BUILT-IN rather than against the
     * literal it currently holds. The claim is "a field the file did not
     * mention keeps the built-in value", so saying it that way means changing
     * the built-in does not falsify a test about inheritance. */
    {
        const nd_theme_palette *b = nd_theme_palette_builtin();

        CHECK_INT(t.palette.blue_bot.r, b->blue_bot.r);
        CHECK_INT(t.palette.sky_top.r, b->sky_top.r);
        CHECK_INT(t.palette.scrim_top_a, b->scrim_top_a);
    }
}

/* An id is the one thing a theme cannot do without: it is what the setting
 * stores. A name is optional and falls back to the id. */
static void test_a_theme_without_an_id_is_refused(void)
{
    nd_theme_info t;
    const char *dir = make_theme("Nameless", "{\"name\":\"No id here\"}");

    CHECK_INT(nd_theme_read(dir, &t), ND_ERR_INVAL);
}

static void test_a_theme_without_a_name_shows_its_id(void)
{
    nd_theme_info t;
    const char *dir = make_theme("Bare", "{\"id\":\"bare\"}");

    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    CHECK_STR(t.name, "bare");
}

/* Malformed JSON is refused outright rather than half-applied: a palette with
 * three of its colours parsed is worse than no theme at all. */
static void test_broken_json_is_refused(void)
{
    nd_theme_info t;
    const char *dir = make_theme("Broken", "{\"id\": \"broken\", ");

    CHECK_INT(nd_theme_read(dir, &t), ND_ERR_INVAL);
}

/* A colour that is not #RRGGBB leaves that field alone and does not fail the
 * theme -- same rule as an absent one, because a typo and an omission should
 * not have different blast radii. */
static void test_a_bad_colour_keeps_the_default_and_the_theme_still_loads(void)
{
    nd_theme_info t;
    const char *dir = make_theme("Sloppy", "{\"id\":\"sloppy\","
                                           "\"palette\":{\"blue_top\":\"red\","
                                           "\"blue_bot\":\"#00FF00\"}}");

    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    /* the built-in survived the bad value, and the good one landed */
    CHECK_INT(t.palette.blue_top.r, nd_theme_palette_builtin()->blue_top.r);
    CHECK_INT(t.palette.blue_bot.g, 0xFF);
}

/* Out of range is clamped, not refused: "300" means "as hard as it goes". */
static void test_an_alpha_is_clamped(void)
{
    nd_theme_info t;
    const char *dir = make_theme("Loud", "{\"id\":\"loud\","
                                         "\"alpha\":{\"scrim_top\":300,\"sheen\":-5}}");

    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    CHECK_INT(t.palette.scrim_top_a, 255);
    CHECK_INT(t.palette.sheen_a, 0);
}

/* What a theme ships is recorded when it is read, so the picker can say so
 * without stat()ing seven paths per keypress. */
static void test_the_optional_parts_are_noticed(void)
{
    nd_theme_info t;
    const char *dir = make_theme("Rich", "{\"id\":\"rich\"}");

    touch(dir, "fonts/ui.ttf");
    touch(dir, "icons/Messages.png");
    touch(dir, "wallpaper.jpg");

    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    CHECK(t.has_font);
    CHECK(!t.has_font_bold);
    CHECK(t.has_icons);
    CHECK(t.has_wallpaper);
    CHECK(!t.has_preview);
}

/* The built-in is always in the list and always first, so an owner can always
 * get back to the stock look. */
static void test_the_list_always_offers_the_built_in_first(void)
{
    nd_theme_info list[ND_THEME_MAX_FOUND];
    size_t n;

    (void)make_theme("Aardvark", "{\"id\":\"aardvark\",\"name\":\"Aardvark\"}");
    n = nd_theme_list(list, ND_THEME_MAX_FOUND);

    CHECK_INT(n, 2);
    CHECK_STR(list[0].id, ND_THEME_ID_BUILTIN);
    CHECK(list[0].builtin);
    CHECK_STR(list[1].id, "aardvark");
    CHECK(!list[1].builtin);
}

/* A theme on a card claiming the built-in's id cannot shadow it. */
static void test_a_theme_cannot_steal_the_built_in_id(void)
{
    nd_theme_info list[ND_THEME_MAX_FOUND];
    size_t n;

    (void)make_theme("Impostor", "{\"id\":\"" ND_THEME_ID_BUILTIN "\",\"name\":\"Not really\"}");
    n = nd_theme_list(list, ND_THEME_MAX_FOUND);

    CHECK_INT(n, 1);
    CHECK_STR(list[0].name, ND_THEME_NAME_BUILTIN);
}

/* A directory that does not parse is skipped, and costs the owner only that
 * one entry rather than the whole picker. */
static void test_one_broken_theme_does_not_cost_the_list(void)
{
    nd_theme_info list[ND_THEME_MAX_FOUND];
    size_t n;

    (void)make_theme("Good", "{\"id\":\"good\",\"name\":\"Good\"}");
    (void)make_theme("Bad", "{ not json");
    n = nd_theme_list(list, ND_THEME_MAX_FOUND);

    CHECK_INT(n, 2);
    CHECK_STR(list[1].id, "good");
}

/* Applying swings the palette pointer, and the copy is what makes it safe to
 * pass a stack temporary. */
static void test_apply_swaps_the_palette_and_keeps_its_own_copy(void)
{
    nd_theme_info t;
    const char *dir = make_theme("Pink", "{\"id\":\"pink\",\"name\":\"Pink\","
                                         "\"palette\":{\"blue_top\":\"#ED538E\"}}");

    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    nd_theme_apply(&t);
    CHECK_INT(nd_theme_pal->blue_top.r, 0xED);
    CHECK_STR(nd_theme_active()->id, "pink");

    /* Scribble on the caller's copy: the active palette must not follow it. */
    t.palette.blue_top = ND_RGB(0, 0, 0);
    CHECK_INT(nd_theme_pal->blue_top.r, 0xED);

    nd_theme_apply(NULL);
    CHECK(nd_theme_pal == nd_theme_palette_builtin());
    CHECK_STR(nd_theme_active()->id, ND_THEME_ID_BUILTIN);
}

/* A setting naming a theme that is not installed draws the stock look and
 * does NOT rewrite the setting -- so putting the card back restores the
 * owner's choice without them re-picking it. */
static void test_a_missing_theme_falls_back_without_forgetting(void)
{
    char got[ND_THEME_ID_MAX];

    CHECK_INT(nd_settings_set(ND_SET_UI_THEME, "gone"), ND_OK);
    nd_theme_load_active();

    CHECK(nd_theme_pal == nd_theme_palette_builtin());
    (void)nd_settings_get_copy(ND_SET_UI_THEME, "", got, sizeof got);
    CHECK_STR(got, "gone");
}

static void test_select_persists_and_applies(void)
{
    char got[ND_THEME_ID_MAX];

    (void)make_theme("Mint", "{\"id\":\"mint\",\"palette\":{\"sky_top\":\"#00FF88\"}}");

    CHECK_INT(nd_theme_select("mint"), ND_OK);
    CHECK_INT(nd_theme_pal->sky_top.g, 0xFF);
    (void)nd_settings_get_copy(ND_SET_UI_THEME, "", got, sizeof got);
    CHECK_STR(got, "mint");

    CHECK_INT(nd_theme_select("nosuch"), ND_ERR_NOTFOUND);
}

/* ------------------------------------------------------------------ *
 * The resource override
 * ------------------------------------------------------------------ */

/* With no theme on, every path comes back exactly as given. This is the case
 * that keeps a stock phone drawing what it always drew. */
static void test_no_theme_overrides_nothing(void)
{
    char out[ND_PATH_MAX];

    nd_theme_apply(NULL);
    CHECK(!nd_theme_resource(ND_PATH_UI_FONT, out, sizeof out));
    CHECK_STR(out, ND_PATH_UI_FONT);
    CHECK(!nd_theme_resource(ND_PATH_APPS_DIR "/Messages/icon.png", out, sizeof out));
    CHECK_STR(out, ND_PATH_APPS_DIR "/Messages/icon.png");
}

/* The four mappings, and the rule that a file has to actually be there. */
static void test_the_resource_mapping(void)
{
    nd_theme_info t;
    char out[ND_PATH_MAX];
    char want[TP_MAX * 2];
    const char *dir = make_theme("Mapped", "{\"id\":\"mapped\"}");

    touch(dir, "fonts/ui.ttf");
    touch(dir, "icons/Messages.png");
    touch(dir, "img/battery/bat-3.png");
    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    nd_theme_apply(&t);

    (void)snprintf(want, sizeof want, "%s/fonts/ui.ttf", dir);
    CHECK(nd_theme_resource(ND_PATH_UI_FONT, out, sizeof out));
    CHECK_STR(out, want);

    (void)snprintf(want, sizeof want, "%s/icons/Messages.png", dir);
    CHECK(nd_theme_resource(ND_PATH_APPS_DIR "/Messages/icon.png", out, sizeof out));
    CHECK_STR(out, want);

    (void)snprintf(want, sizeof want, "%s/img/battery/bat-3.png", dir);
    CHECK(nd_theme_resource("/NeoDCT/System/ui/resources/img/battery/bat-3.png", out, sizeof out));
    CHECK_STR(out, want);

    /* The theme has an icons/ directory but no icon for THIS app: the
     * per-file test is what makes a partial theme legal, and a partial theme
     * is the normal case. */
    CHECK(!nd_theme_resource(ND_PATH_APPS_DIR "/Clock/icon.png", out, sizeof out));
    CHECK_STR(out, ND_PATH_APPS_DIR "/Clock/icon.png");

    /* It ships no bold face, so the bold path is the system's. */
    CHECK(!nd_theme_resource(ND_PATH_UI_FONT_BOLD, out, sizeof out));
    CHECK_STR(out, ND_PATH_UI_FONT_BOLD);

    /* The pixel face is deliberately not themeable: the boot bar's glyph
     * tables are baked out of it and it is pinned by sha256. */
    CHECK(!nd_theme_resource(ND_PATH_FONT, out, sizeof out));
    CHECK_STR(out, ND_PATH_FONT);
}

/* An app installed from a card is themed too, because the icon rule matches
 * the SHAPE of the path and not its root. A theme therefore covers an app
 * that did not exist when the theme was written. */
static void test_an_installed_apps_icon_is_themed_by_name(void)
{
    nd_theme_info t;
    char out[ND_PATH_MAX];
    char want[TP_MAX * 2];
    const char *dir = make_theme("Wide", "{\"id\":\"wide\"}");

    touch(dir, "icons/Bible.png");
    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    nd_theme_apply(&t);

    (void)snprintf(want, sizeof want, "%s/icons/Bible.png", dir);
    CHECK(nd_theme_resource(ND_PATH_USER_APPS_DIR "/Bible/icon.png", out, sizeof out));
    CHECK_STR(out, want);
}

int main(void)
{
    CHECK_INT(nd_settings_init(), ND_OK);

    RUN(test_the_built_in_is_active_before_anything_is_loaded);
    RUN(test_an_absent_colour_keeps_the_built_in_value);
    RUN(test_a_theme_without_an_id_is_refused);
    RUN(test_a_theme_without_a_name_shows_its_id);
    RUN(test_broken_json_is_refused);
    RUN(test_a_bad_colour_keeps_the_default_and_the_theme_still_loads);
    RUN(test_an_alpha_is_clamped);
    RUN(test_the_optional_parts_are_noticed);
    RUN(test_the_list_always_offers_the_built_in_first);
    RUN(test_a_theme_cannot_steal_the_built_in_id);
    RUN(test_one_broken_theme_does_not_cost_the_list);
    RUN(test_apply_swaps_the_palette_and_keeps_its_own_copy);
    RUN(test_a_missing_theme_falls_back_without_forgetting);
    RUN(test_select_persists_and_applies);
    RUN(test_no_theme_overrides_nothing);
    RUN(test_the_resource_mapping);
    RUN(test_an_installed_apps_icon_is_themed_by_name);

    nd_theme_apply(NULL);
    pt_cleanup();
    return pt_report("test_theme");
}
