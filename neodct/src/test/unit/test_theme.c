/* test_theme.c -- the theme loader: what a theme file is allowed to be, and
 * what happens when it is not that.
 *
 * The palette itself is covered by the GOLDEN FRAMES rather than by anything
 * here, and deliberately: the claim worth defending is "the built-in defaults
 * render the pixels they always did", and only a rendered frame can say that.
 * What this file covers is the part a frame cannot see -- parsing, the
 * registry's precedence, the fallbacks, and the resource mapping.
 */

#include <stdlib.h>
#include <string.h>

#include "nd_image.h"
#include "nd_proc.h"
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

/* The launch transition is decoration like gloss: off unless a theme turns
 * it on, so the stock phone and any theme that predates the key launch
 * instantly. */
static void test_the_launch_transition_is_opt_in(void)
{
    nd_theme_info t;

    CHECK(!nd_theme_style_builtin()->launch_anim);
    /* One at a time: make_theme() hands back the same static buffer. */
    CHECK_INT(nd_theme_read(make_theme("Quiet", "{\"id\":\"quiet\"}"), &t), ND_OK);
    CHECK(!t.style.launch_anim);
    CHECK_INT(nd_theme_read(make_theme("Glass", "{\"id\":\"glass\","
                                                "\"style\":{\"launch_anim\":true}}"),
                            &t),
              ND_OK);
    CHECK(t.style.launch_anim);
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

/* A confined app cannot read settings.prop, so the core says which theme is
 * on. When it has spoken, that is the answer -- even against a setting the
 * app happens to be able to read -- or an installed app draws the built-in
 * look inside a themed phone, which is the bug this closes. */
static void test_the_cores_word_beats_the_setting(void)
{
    (void)make_theme("Mint", "{\"id\":\"mint\",\"palette\":{\"sky_top\":\"#00FF88\"}}");
    CHECK_INT(nd_settings_set(ND_SET_UI_THEME, ND_THEME_ID_BUILTIN), ND_OK);

    CHECK_INT(setenv(ND_ENV_UI_THEME, "mint", 1), 0);
    nd_theme_load_active();
    CHECK_STR(nd_theme_active()->id, "mint");
    CHECK_INT(nd_theme_pal->sky_top.g, 0xFF);

    /* And absent means "no core told me": the setting decides again. */
    CHECK_INT(unsetenv(ND_ENV_UI_THEME), 0);
    nd_theme_load_active();
    CHECK_STR(nd_theme_active()->id, ND_THEME_ID_BUILTIN);
}

/* The core asks this after every app exit. It must say yes when the owner
 * picked something else, and it must NOT keep saying yes for a theme that
 * cannot be found -- the core would reload its fonts on every app exit. */
static void test_stale_means_the_choice_moved(void)
{
    (void)make_theme("Mint", "{\"id\":\"mint\"}");

    CHECK_INT(nd_settings_set(ND_SET_UI_THEME, "gone"), ND_OK);
    nd_theme_load_active();
    CHECK(nd_theme_pal == nd_theme_palette_builtin());
    CHECK(!nd_theme_is_stale());

    CHECK_INT(nd_settings_set(ND_SET_UI_THEME, "mint"), ND_OK);
    CHECK(nd_theme_is_stale());
    nd_theme_load_active();
    CHECK(!nd_theme_is_stale());
    CHECK_STR(nd_theme_active()->id, "mint");

    /* Selecting is loading: the process that chose it is wearing it. */
    CHECK_INT(nd_theme_select(ND_THEME_ID_BUILTIN), ND_OK);
    CHECK(!nd_theme_is_stale());
}

/* A theme's picture arrives with it and LEAVES with it. Going back to Classic
 * used to keep the last theme's wallpaper, so the stock look wore part of a
 * theme the owner had just taken off. A picture the owner chose is theirs and
 * is never touched. */
static void test_a_themes_wallpaper_leaves_with_it(void)
{
    char got[ND_PATH_MAX];
    char want[TP_MAX];
    const char *dir = make_theme("Sunny", "{\"id\":\"sunny\"}");

    touch(dir, "wallpaper.jpg");
    (void)snprintf(want, sizeof want, "%s/wallpaper.jpg", dir);

    CHECK_INT(nd_theme_select("sunny"), ND_OK);
    (void)nd_settings_get_copy(ND_SET_UI_WALLPAPER, "", got, sizeof got);
    CHECK_STR(got, want);
    CHECK(nd_theme_owns_path(got));

    CHECK_INT(nd_theme_select(ND_THEME_ID_BUILTIN), ND_OK);
    (void)nd_settings_get_copy(ND_SET_UI_WALLPAPER, "", got, sizeof got);
    CHECK_STR(got, ND_SET_UI_WALLPAPER_DFLT);

    CHECK_INT(nd_settings_set(ND_SET_UI_WALLPAPER, "/NeoDCT/System/wallpapers/Grasslands.jpg"),
              ND_OK);
    CHECK(!nd_theme_owns_path("/NeoDCT/System/wallpapers/Grasslands.jpg"));
    CHECK_INT(nd_theme_select(ND_THEME_ID_BUILTIN), ND_OK);
    (void)nd_settings_get_copy(ND_SET_UI_WALLPAPER, "", got, sizeof got);
    CHECK_STR(got, "/NeoDCT/System/wallpapers/Grasslands.jpg");
}

/* ------------------------------------------------------------------ *
 * The ground
 * ------------------------------------------------------------------ */

/* The classic look's bars and panels ARE the background. Over a wallpaper,
 * filling them in the background colour is a black band across the picture,
 * so a title strip drawn there must leave the picture alone. A theme whose
 * bars are a colour of their own still gets them painted. */
static void test_a_bar_that_is_the_ground_is_not_painted(void)
{
    nd_image *img;
    nd_theme_info t;
    nd_color px;
    const char *dir;

    nd_theme_apply(NULL);
    CHECK(!nd_theme_bars_painted());
    CHECK(!nd_theme_panels_painted());
    CHECK_INT(nd_theme_plate_bar(0).body_a, 0);
    CHECK_INT(nd_theme_plate_glass(4).body_a, 0);
    CHECK_INT(nd_theme_panel_a(240u), 0);

    img = nd_image_new_filled(40, 40, ND_PIXFMT_RGB888, ND_RGB(0x30, 0x80, 0x20));
    CHECK(img != NULL);
    if (img == NULL)
        return;
    (void)nd_theme_titlebar(img, NULL, 40, 20, NULL, NULL, NULL, NULL);
    px = nd_image_get_px(img, 20, 10);
    CHECK_INT(px.r, 0x30);
    CHECK_INT(px.g, 0x80);
    CHECK_INT(px.b, 0x20);

    dir = make_theme("Barred", "{\"id\":\"barred\",\"palette\":{\"bar_top\":\"#2A9BE8\","
                               "\"bar_bot\":\"#0A4A9B\",\"glass_top\":\"#F2F9FF\"}}");
    CHECK_INT(nd_theme_read(dir, &t), ND_OK);
    nd_theme_apply(&t);
    CHECK(nd_theme_bars_painted());
    CHECK(nd_theme_panels_painted());
    CHECK_INT(nd_theme_plate_bar(0).body_a, 255);
    CHECK_INT(nd_theme_panel_a(240u), 240);
    (void)nd_theme_titlebar(img, NULL, 40, 20, NULL, NULL, NULL, NULL);
    px = nd_image_get_px(img, 20, 2);
    CHECK(px.r != 0x30 || px.g != 0x80 || px.b != 0x20);

    nd_image_free(img);
    nd_theme_apply(NULL);
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
    RUN(test_the_launch_transition_is_opt_in);
    RUN(test_the_list_always_offers_the_built_in_first);
    RUN(test_a_theme_cannot_steal_the_built_in_id);
    RUN(test_one_broken_theme_does_not_cost_the_list);
    RUN(test_apply_swaps_the_palette_and_keeps_its_own_copy);
    RUN(test_a_missing_theme_falls_back_without_forgetting);
    RUN(test_select_persists_and_applies);
    RUN(test_the_cores_word_beats_the_setting);
    RUN(test_stale_means_the_choice_moved);
    RUN(test_a_themes_wallpaper_leaves_with_it);
    RUN(test_a_bar_that_is_the_ground_is_not_painted);
    RUN(test_no_theme_overrides_nothing);
    RUN(test_the_resource_mapping);
    RUN(test_an_installed_apps_icon_is_themed_by_name);

    nd_theme_apply(NULL);
    pt_cleanup();
    return pt_report("test_theme");
}
