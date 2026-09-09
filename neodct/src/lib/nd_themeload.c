/* nd_themeload.c -- the palette as a file, and the file as a directory.
 *
 * nd_theme.c draws. This draws nothing: it decides what nd_theme.c draws
 * WITH, which is the whole of "the look is patchable at runtime".
 *
 * ============ WHY THIS IS A SECOND FILE ============
 *
 * nd_theme.c is the render path and has one rule -- no allocation, no I/O,
 * nothing that can fail. Everything here opens files, parses JSON and walks
 * directories, and it runs at most a handful of times in a process's life.
 * Two files keep the reviewer's question "can this be called per frame?"
 * answerable by looking at the filename.
 *
 * ============ THE DEFAULTS ARE THE OLD #defines, EXACTLY ============
 *
 * Every value in builtin_palette below is byte-for-byte the literal that used
 * to be compiled in. That is not a coincidence to be maintained by care: the
 * golden frames are the check, and they are compared against frames captured
 * before this file existed. A typo here moves a frame and the suite says so.
 */

#include "nd_theme.h"

#include <dirent.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "nd_json.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_settings.h"

/* ------------------------------------------------------------------ *
 * The built-in look
 * ------------------------------------------------------------------ */

/* ============ THE BUILT-IN IS THE PLAIN ONE ============
 *
 * White type on black, one white rule under the title, a white lozenge with
 * black type for the selected row. That is the phone's own face -- the
 * Nokia-style look this OS is an imitation of -- and it is what an owner gets
 * with nothing installed.
 *
 * It is the DEFAULT rather than a theme for a reason that is not nostalgia:
 * every field a theme file omits falls back to these values, so a half-
 * written theme lands on the honest, cheap, legible look instead of
 * inheriting somebody else's gloss and coming out as a black glass plate.
 * The decoration is opt-in, and Frutiger Aero is the theme that opts in.
 *
 * Pure black and pure white on purpose. This panel is 240x175 and the classic
 * face gets its legibility from maximum contrast; the navy-instead-of-black
 * reasoning in the glass theme is about type on a blue-white gradient, and
 * there is no gradient here. */
static const nd_theme_palette builtin_palette = {
    /* the signature colour: the selection lozenge, a progress fill. White,
     * because the classic selection is an inverted row. */
    ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0xFF, 0xFF, 0xFF),
    ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0xFF, 0xFF, 0xFF),
    /* glass: the panel content sits on -- black, i.e. nothing at all */
    ND_RGB(0x00, 0x00, 0x00), ND_RGB(0x00, 0x00, 0x00),
    /* chrome: the rules and the scrollbar. White line art. */
    ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0x80, 0x80, 0x80),
    /* the background */
    ND_RGB(0x00, 0x00, 0x00), ND_RGB(0x00, 0x00, 0x00),
    /* green, amber, red: the battery and the warnings, which stay legible
     * colours even here -- a red fault has to read as one. The WARNING LINE
     * is warn_ink below and not this pair; see the header. */
    ND_RGB(0x2E, 0xCC, 0x40), ND_RGB(0x2E, 0xCC, 0x40),
    ND_RGB(0xFF, 0xB0, 0x00), ND_RGB(0xFF, 0xB0, 0x00), ND_RGB(0xFF, 0x41, 0x36),
    ND_RGB(0xFF, 0x41, 0x36),
    /* the bars: the title strip and the softkey strip, both the background */
    ND_RGB(0x00, 0x00, 0x00), ND_RGB(0x00, 0x00, 0x00), ND_RGB(0xFF, 0xFF, 0xFF),
    /* type standing on the selection: black, because the lozenge is white */
    ND_RGB(0x00, 0x00, 0x00),
    /* the warning line, pure red -- what the phone has always drawn it in */
    ND_RGB(0xFF, 0x00, 0x00),
    /* Ink. ink_dark is "type on a glass panel", and this theme's glass IS the
     * black background -- there is no light plate anywhere in it -- so the
     * type on one has to be WHITE. Leaving it black is how the calculator's
     * readout came out invisible: a black panel with a white border and black
     * digits inside it. */
    ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0xA0, 0xA0, 0xA0),
    /* the type shadow and sheen, and the scrim's ink. Unused while
     * style.type_shadow and style.scrim are off, and named anyway so that a
     * theme turning them on without restating them gets something sane. */
    ND_RGB(0x00, 0x00, 0x00), ND_RGB(0xFF, 0xFF, 0xFF), ND_RGB(0x00, 0x00, 0x00),
    /* the five coverages */
    96u, 0u, 70u, 150u, 110u,
};

/* And the structure that goes with it: nothing on but the pixel face. */
static const nd_theme_style builtin_style = {
    false, /* gloss         */
    false, /* bevel         */
    false, /* gradients     */
    false, /* round         */
    false, /* type_shadow   */
    false, /* plate_shadow  */
    false, /* bevel_divider */
    false, /* icon_glow     */
    false, /* reflection    */
    false, /* scrim         */
    true,  /* pixel_font    */
    30u,   /* wallpaper_dim     -- no scrim, so the picture itself gives way */
    75u,   /* app_wallpaper_dim -- and further still inside an app */
};

/* The active theme, and the pointer everything draws through.
 *
 * `active` starts as the built-in with an empty dir, so nd_theme_resource()
 * answers "not overridden" before anything has been loaded and the OS draws
 * correctly in a process that never calls nd_theme_load_active() at all --
 * which is what nd-shoot and every unit test do. */
static nd_theme_info active;
static bool active_ready;

const nd_theme_palette *nd_theme_pal = &builtin_palette;
const nd_theme_style *nd_theme_style_of = &builtin_style;

const nd_theme_palette *nd_theme_palette_builtin(void)
{
    return &builtin_palette;
}

const nd_theme_style *nd_theme_style_builtin(void)
{
    return &builtin_style;
}

/* The built-in as a whole record. Not a constant, because `palette` has to be
 * copied in from builtin_palette and C89-style static init of a nested struct
 * literal here would restate all twenty-seven values a second time -- which is
 * exactly the duplication the golden frames caught last time. */
static void builtin_info(nd_theme_info *out)
{
    memset(out, 0, sizeof *out);
    (void)nd_strlcpy(out->id, ND_THEME_ID_BUILTIN, sizeof out->id);
    (void)nd_strlcpy(out->name, ND_THEME_NAME_BUILTIN, sizeof out->name);
    (void)nd_strlcpy(out->version, "built in", sizeof out->version);
    (void)nd_strlcpy(out->desc, "White type on black in the phone's own pixel typeface. Flat, "
                                "square-cornered and high contrast.",
                     sizeof out->desc);
    out->builtin = true;
    out->palette = builtin_palette;
    out->style = builtin_style;
}

/* ------------------------------------------------------------------ *
 * Parsing
 * ------------------------------------------------------------------ */

static bool hex_nib(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9')
        *out = (uint8_t)(c - '0');
    else if (c >= 'a' && c <= 'f')
        *out = (uint8_t)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F')
        *out = (uint8_t)(c - 'A' + 10);
    else
        return false;
    return true;
}

/* "#RRGGBB", and nothing else.
 *
 * No named colours and no rgb() form: a theme file is written by a person with
 * a picker in front of them, every one of them emits hex, and each extra
 * spelling is a way for two themes to disagree about "red". A malformed value
 * leaves *out alone, which is how an unknown key and a broken one come out the
 * same -- the field keeps its built-in value. */
static bool parse_colour(const nd_json_val *v, nd_color *out)
{
    const char *s = NULL;
    uint8_t hi = 0;
    uint8_t lo = 0;
    uint8_t rgb[3];
    size_t i;

    if (!nd_json_str(v, &s) || s == NULL || s[0] != '#' || strlen(s) != 7u)
        return false;
    for (i = 0; i < 3u; i++) {
        if (!hex_nib(s[1 + i * 2], &hi) || !hex_nib(s[2 + i * 2], &lo))
            return false;
        rgb[i] = (uint8_t)((hi << 4) | lo);
    }
    *out = ND_RGB(rgb[0], rgb[1], rgb[2]);
    return true;
}

static void read_colour(const nd_json_val *obj, const char *key, nd_color *out)
{
    const nd_json_val *v = nd_json_get(obj, key);

    if (v != NULL && !parse_colour(v, out))
        nd_log_err(ND_LOG_UI, "theme: %s is not a #RRGGBB colour; keeping the default", key);
}

/* A 0..255 coverage. Out-of-range is clamped rather than refused: a theme
 * asking for 300 means "as hard as it goes" and there is nothing to gain by
 * making the owner's phone refuse to draw over it. */
static void read_alpha(const nd_json_val *obj, const char *key, uint8_t *out)
{
    const nd_json_val *v = nd_json_get(obj, key);
    int64_t n = 0;

    if (v == NULL)
        return;
    if (!nd_json_int(v, &n)) {
        nd_log_err(ND_LOG_UI, "theme: %s is not a number; keeping the default", key);
        return;
    }
    if (n < 0)
        n = 0;
    if (n > 255)
        n = 255;
    *out = (uint8_t)n;
}

static void read_str(const nd_json_val *obj, const char *key, char *out, size_t out_sz)
{
    const nd_json_val *v = nd_json_get(obj, key);
    const char *s = NULL;

    if (v != NULL && nd_json_str(v, &s) && s != NULL)
        (void)nd_strlcpy(out, s, out_sz);
}

/* ============ VIRTUAL PATHS, AND WHY THE RESOLVING JOIN IS WRONG HERE =====
 *
 * nd_path_join() RESOLVES: it prepends ND_ROOT and hands back a real
 * filesystem path. That is right for a caller about to open the file and
 * wrong for everything in this file, because a theme's directory is carried
 * around in nd_theme_info.dir, probed with nd_path_is_file() -- which resolves
 * again -- and handed to nd_theme_resource()'s callers, which resolve a third
 * time. Two resolutions produce "<root><root>/NeoDCT/..." and every probe then
 * answers no, which presents as "the theme is not installed" on a phone whose
 * theme is sitting right there. It cost an afternoon to find.
 *
 * So paths here are VIRTUAL end to end, exactly like the constants in
 * nd_paths.h, and the single resolution happens at the one place that opens a
 * file: nd_theme_read()'s call to nd_json_parse_file(). */
static nd_err join_virtual(char *out, size_t out_sz, const char *dir, const char *child)
{
    return nd_snprintf(out, out_sz, "%s/%s", dir, child);
}

/* Does <dir>/<child> exist? Used only to record what a theme ships, so an
 * error reads as absent. */
static bool sub_exists(const char *dir, const char *child, bool want_dir)
{
    char path[ND_PATH_MAX];

    if (join_virtual(path, sizeof path, dir, child) != ND_OK)
        return false;
    return want_dir ? nd_path_is_dir(path) : nd_path_is_file(path);
}

/* The palette, keyed exactly as the struct's fields are named. One table so
 * that adding a colour is one line here and one line in the header, rather
 * than a function that grows a paragraph. */
static void read_palette(const nd_json_val *pal, nd_theme_palette *p)
{
    read_colour(pal, "blue_hi", &p->blue_hi);
    read_colour(pal, "blue_top", &p->blue_top);
    read_colour(pal, "blue_mid", &p->blue_mid);
    read_colour(pal, "blue_bot", &p->blue_bot);
    read_colour(pal, "blue_deep", &p->blue_deep);
    read_colour(pal, "glass_top", &p->glass_top);
    read_colour(pal, "glass_bot", &p->glass_bot);
    read_colour(pal, "chrome_hi", &p->chrome_hi);
    read_colour(pal, "chrome_top", &p->chrome_top);
    read_colour(pal, "chrome_bot", &p->chrome_bot);
    read_colour(pal, "sky_top", &p->sky_top);
    read_colour(pal, "sky_bot", &p->sky_bot);
    read_colour(pal, "green_top", &p->green_top);
    read_colour(pal, "green_bot", &p->green_bot);
    read_colour(pal, "amber_top", &p->amber_top);
    read_colour(pal, "amber_bot", &p->amber_bot);
    read_colour(pal, "red_top", &p->red_top);
    read_colour(pal, "red_bot", &p->red_bot);
    read_colour(pal, "bar_top", &p->bar_top);
    read_colour(pal, "bar_bot", &p->bar_bot);
    read_colour(pal, "bar_ink", &p->bar_ink);
    read_colour(pal, "sel_ink", &p->sel_ink);
    read_colour(pal, "warn_ink", &p->warn_ink);
    read_colour(pal, "ink_dark", &p->ink_dark);
    read_colour(pal, "ink_light", &p->ink_light);
    read_colour(pal, "ink_muted", &p->ink_muted);
    read_colour(pal, "text_shadow", &p->text_shadow);
    read_colour(pal, "text_sheen", &p->text_sheen);
    read_colour(pal, "scrim_ink", &p->scrim_ink);
}

static void read_flag(const nd_json_val *obj, const char *key, bool *out)
{
    const nd_json_val *v = nd_json_get(obj, key);
    bool b = false;

    if (v == NULL)
        return;
    if (!nd_json_bool(v, &b)) {
        nd_log_err(ND_LOG_UI, "theme: style.%s is not true or false; keeping the default", key);
        return;
    }
    *out = b;
}

/* The structural switches. Named exactly as the struct's fields, and all of
 * them default OFF -- see nd_theme_style in the header for why the decoration
 * is opt-in rather than opt-out. */
static void read_style(const nd_json_val *st, nd_theme_style *y)
{
    read_flag(st, "gloss", &y->gloss);
    read_flag(st, "bevel", &y->bevel);
    read_flag(st, "gradients", &y->gradients);
    read_flag(st, "round", &y->round);
    read_flag(st, "type_shadow", &y->type_shadow);
    read_flag(st, "plate_shadow", &y->plate_shadow);
    read_flag(st, "bevel_divider", &y->bevel_divider);
    read_flag(st, "icon_glow", &y->icon_glow);
    read_flag(st, "reflection", &y->reflection);
    read_flag(st, "scrim", &y->scrim);
    read_flag(st, "pixel_font", &y->pixel_font);
    {
        static const struct {
            const char *key;
            size_t off;
        } DIMS[] = {
            {"wallpaper_dim", offsetof(nd_theme_style, wallpaper_dim)},
            {"app_wallpaper_dim", offsetof(nd_theme_style, app_wallpaper_dim)},
        };
        size_t i;

        for (i = 0u; i < ND_ARRAY_LEN(DIMS); i++) {
            const nd_json_val *v = nd_json_get(st, DIMS[i].key);
            int64_t n = 0;

            if (v == NULL || !nd_json_int(v, &n))
                continue;
            if (n < 0)
                n = 0;
            if (n > 100)
                n = 100;
            *((uint8_t *)((char *)y + DIMS[i].off)) = (uint8_t)n;
        }
    }
}

nd_err nd_theme_read(const char *dir, nd_theme_info *out)
{
    char manifest[ND_PATH_MAX];
    nd_json_doc *doc = NULL;
    const nd_json_val *root;
    const nd_json_val *pal;
    nd_theme_info t;
    char err[128];
    nd_err rc;

    if (dir == NULL || out == NULL)
        return ND_ERR_INVAL;
    if (join_virtual(manifest, sizeof manifest, dir, ND_THEME_MANIFEST) != ND_OK)
        return ND_ERR_TOOLONG;
    if (!nd_path_is_file(manifest))
        return ND_ERR_NOTFOUND;

    /* THE VIRTUAL PATH, NOT A RESOLVED ONE. nd_json_parse_file() calls
     * nd_path_resolve() itself, so handing it an already-resolved path applies
     * ND_ROOT twice and fopen(2) fails with ND_ERR_IO on a file that is
     * plainly there -- which reads, three layers up, as "the theme is not
     * installed". Unlike nd_font_load(), which really does want a real path,
     * every reader in nd_json.h resolves for you. */
    err[0] = '\0';
    rc = nd_json_parse_file(manifest, &doc, err, sizeof err);
    if (rc != ND_OK) {
        nd_log_err(ND_LOG_UI, "theme: %s is not readable: %s", manifest,
                   err[0] != '\0' ? err : nd_strerror(rc));
        return ND_ERR_INVAL;
    }
    root = nd_json_root(doc);
    if (nd_json_type_of(root) != ND_JSON_OBJECT) {
        nd_json_free(doc);
        nd_log_err(ND_LOG_UI, "theme: %s is not a JSON object", manifest);
        return ND_ERR_INVAL;
    }

    /* Start from the built-in and let the file override what it mentions. A
     * theme that names three colours is therefore complete, and one that names
     * a colour this build has never heard of is ignored rather than fatal --
     * which is what lets a theme written for 0.7 be installed on 0.6 and
     * simply look like less. */
    builtin_info(&t);
    t.builtin = false;
    t.id[0] = '\0';
    t.name[0] = '\0';
    t.version[0] = '\0';
    t.desc[0] = '\0';

    read_str(root, "id", t.id, sizeof t.id);
    read_str(root, "name", t.name, sizeof t.name);
    read_str(root, "author", t.author, sizeof t.author);
    read_str(root, "version", t.version, sizeof t.version);
    read_str(root, "description", t.desc, sizeof t.desc);

    pal = nd_json_get(root, "palette");
    if (nd_json_type_of(pal) == ND_JSON_OBJECT)
        read_palette(pal, &t.palette);
    pal = nd_json_get(root, "style");
    if (nd_json_type_of(pal) == ND_JSON_OBJECT)
        read_style(pal, &t.style);
    pal = nd_json_get(root, "alpha");
    if (nd_json_type_of(pal) == ND_JSON_OBJECT) {
        read_alpha(pal, "scrim_top", &t.palette.scrim_top_a);
        read_alpha(pal, "scrim_bot", &t.palette.scrim_bot_a);
        read_alpha(pal, "appsel_scrim", &t.palette.appsel_scrim_a);
        read_alpha(pal, "shadow", &t.palette.shadow_a);
        read_alpha(pal, "sheen", &t.palette.sheen_a);
    }
    nd_json_free(doc);

    /* An id is the only thing a theme cannot do without: it is what the
     * setting stores and what the picker matches on. A theme with no name
     * shows its id, which is ugly and still usable. */
    if (t.id[0] == '\0') {
        nd_log_err(ND_LOG_UI, "theme: %s has no \"id\"; ignoring it", manifest);
        return ND_ERR_INVAL;
    }
    if (t.name[0] == '\0')
        (void)nd_strlcpy(t.name, t.id, sizeof t.name);

    (void)nd_strlcpy(t.dir, dir, sizeof t.dir);
    t.has_font = sub_exists(dir, "fonts/ui.ttf", false);
    t.has_font_bold = sub_exists(dir, "fonts/ui-bold.ttf", false);
    t.has_icons = sub_exists(dir, "icons", true);
    t.has_img = sub_exists(dir, "img", true);
    t.has_wallpaper = sub_exists(dir, "wallpaper.jpg", false);
    t.has_preview = sub_exists(dir, ND_THEME_PREVIEW, false);

    *out = t;
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * The registry
 * ------------------------------------------------------------------ */

static size_t scan_dir(const char *root, nd_theme_info *out, size_t max, size_t n)
{
    char resolved[ND_PATH_MAX];
    struct dirent *e;
    DIR *d;

    if (n >= max)
        return n;
    if (nd_path_resolve(resolved, sizeof resolved, root) != ND_OK)
        return n;
    d = opendir(resolved);
    if (d == NULL)
        return n;

    while (n < max && (e = readdir(d)) != NULL) {
        char dir[ND_PATH_MAX];
        nd_theme_info t;
        size_t i;
        bool dup = false;

        if (e->d_name[0] == '.')
            continue;
        if (join_virtual(dir, sizeof dir, root, e->d_name) != ND_OK)
            continue;
        if (!nd_path_is_dir(dir))
            continue;
        if (nd_theme_read(dir, &t) != ND_OK)
            continue;

        /* First id wins, and the built-in is added first, so a card cannot
         * shadow "classic" and leave the owner with no way back to the stock
         * look. The same rule makes the system themes beat the card's. */
        for (i = 0; i < n; i++) {
            if (strcmp(out[i].id, t.id) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            /* out[i].dir is empty for the built-in, which is the case that
             * actually happens -- a card carrying a "classic" -- so it is named
             * rather than printed as a blank path. */
            nd_log(ND_LOG_UI, "theme: %s is already %s; ignoring %s", t.id,
                   out[i].builtin ? "the built-in look" : out[i].dir, dir);
            continue;
        }
        out[n++] = t;
    }
    (void)closedir(d);
    return n;
}

/* Insertion sort by name over the range [from, n). Small n, and it keeps the
 * two roots in their own blocks rather than interleaving them. */
static void sort_by_name(nd_theme_info *a, size_t from, size_t n)
{
    size_t i;

    for (i = from + 1; i < n; i++) {
        nd_theme_info key = a[i];
        size_t j = i;

        while (j > from && strcmp(a[j - 1].name, key.name) > 0) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

size_t nd_theme_list(nd_theme_info *out, size_t max)
{
    size_t n = 0;
    size_t after_sys;

    if (out == NULL || max == 0u)
        return 0;

    /* The built-in is always first and never sorted, because it is the one
     * entry whose position the owner should be able to rely on. */
    builtin_info(&out[0]);
    n = 1;

    after_sys = scan_dir(ND_PATH_THEMES_DIR, out, max, n);
    sort_by_name(out, n, after_sys);
    n = scan_dir(ND_PATH_USER_THEMES_DIR, out, max, after_sys);
    sort_by_name(out, after_sys, n);
    return n;
}

bool nd_theme_find(const char *id, nd_theme_info *out)
{
    nd_theme_info list[ND_THEME_MAX_FOUND];
    size_t n;
    size_t i;

    if (id == NULL || id[0] == '\0')
        return false;
    n = nd_theme_list(list, ND_THEME_MAX_FOUND);
    for (i = 0; i < n; i++) {
        if (strcmp(list[i].id, id) == 0) {
            if (out != NULL)
                *out = list[i];
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------ *
 * Applying
 * ------------------------------------------------------------------ */

void nd_theme_apply(const nd_theme_info *t)
{
    if (t == NULL) {
        builtin_info(&active);
        nd_theme_pal = &builtin_palette;
        nd_theme_style_of = &builtin_style;
        active_ready = true;
        return;
    }
    /* The copy is the whole safety of this: `active` outlives the caller's
     * stack temporary, and nd_theme_pal points into `active` rather than into
     * whatever the picker had on its stack when it previewed. */
    active = *t;
    nd_theme_pal = t->builtin ? &builtin_palette : &active.palette;
    nd_theme_style_of = t->builtin ? &builtin_style : &active.style;
    active_ready = true;
}

const nd_theme_info *nd_theme_active(void)
{
    if (!active_ready) {
        builtin_info(&active);
        active_ready = true;
    }
    return &active;
}

void nd_theme_load_active(void)
{
    char id[ND_THEME_ID_MAX];
    nd_theme_info t;

    if (nd_settings_get_copy(ND_SET_UI_THEME, ND_SET_UI_THEME_DFLT, id, sizeof id) != ND_OK ||
        id[0] == '\0') {
        nd_theme_apply(NULL);
        return;
    }
    if (strcmp(id, ND_THEME_ID_BUILTIN) == 0) {
        nd_theme_apply(NULL);
        return;
    }
    if (!nd_theme_find(id, &t)) {
        /* The card is out, or the theme was deleted. Say so once and draw the
         * stock look; the setting is deliberately NOT rewritten, so putting
         * the card back restores the owner's choice without them re-picking
         * it. */
        nd_log_err(ND_LOG_UI, "theme: \"%s\" is not installed; using the built-in look", id);
        nd_theme_apply(NULL);
        return;
    }
    nd_theme_apply(&t);
    nd_log(ND_LOG_UI, "Theme: %s (%s).", t.name, t.id);
}

nd_err nd_theme_select(const char *id)
{
    nd_theme_info t;
    nd_err rc;

    if (id == NULL || id[0] == '\0')
        return ND_ERR_INVAL;
    if (strcmp(id, ND_THEME_ID_BUILTIN) != 0 && !nd_theme_find(id, &t))
        return ND_ERR_NOTFOUND;

    rc = nd_settings_set(ND_SET_UI_THEME, id);
    if (rc != ND_OK)
        return rc;

    if (strcmp(id, ND_THEME_ID_BUILTIN) == 0)
        nd_theme_apply(NULL);
    else
        nd_theme_apply(&t);

    /* ============ AND THE WALLPAPER GOES WITH IT ============
     *
     * A theme that recolours the chrome and leaves the owner's old background
     * behind has not changed the look, it has broken it: the wallpaper is the
     * largest coloured object on the home screen and a pink interface over a
     * blue sky reads as a bug rather than a theme.
     *
     * So picking a theme writes the wallpaper setting too, WHEN THE THEME
     * SHIPS ONE -- a recolour that ships no picture leaves the owner's
     * choice alone, which is the common case and the one where overwriting
     * would be rude.
     *
     * This is a real cost and it is stated rather than hidden: the previous
     * wallpaper is not remembered, and an owner who had set a photograph gets
     * it back by choosing it again in Settings -> Wallpaper. Remembering it
     * would mean a second setting whose only job is to be stale as soon as
     * the owner picks a wallpaper by hand, and the picker already tells them
     * what applying a theme does. */
    {
        char wp[ND_PATH_MAX];

        if (nd_theme_wallpaper(wp, sizeof wp)) {
            if (nd_settings_set(ND_SET_UI_WALLPAPER, wp) != ND_OK)
                nd_log_err(ND_LOG_UI, "theme: %s applied, but its wallpaper could not be set", id);
        }
    }
    return ND_OK;
}

/* ------------------------------------------------------------------ *
 * Resource override
 * ------------------------------------------------------------------ */

/* The last path component, or NULL when there is none. */
static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

/* "<anything>/apps/<Name>/icon.png" -> "<Name>", written into out.
 *
 * By SHAPE and not by root, so this catches the system apps, the engineering
 * apps and the card's apps alike -- which is what lets a theme cover an app
 * the owner installed after the theme was written. */
static bool app_icon_name(const char *path, char *out, size_t out_sz)
{
    const char *slash;
    const char *dir_end;
    const char *dir_start;
    size_t len;

    if (strcmp(base_name(path), "icon.png") != 0)
        return false;
    slash = strrchr(path, '/');
    if (slash == NULL || slash == path)
        return false;
    dir_end = slash;
    dir_start = dir_end - 1;
    while (dir_start > path && *dir_start != '/')
        dir_start--;
    if (*dir_start != '/')
        return false;
    dir_start++;
    len = (size_t)(dir_end - dir_start);
    if (len == 0u || len + 1u > out_sz)
        return false;
    memcpy(out, dir_start, len);
    out[len] = '\0';
    return true;
}

/* Does `path` start with `prefix`? Returns the remainder, or NULL. */
static const char *after_prefix(const char *path, const char *prefix)
{
    size_t n = strlen(prefix);

    return (strncmp(path, prefix, n) == 0) ? path + n : NULL;
}

/* The longest app directory name a theme can carry an icon for. Matches
 * ND_NAP_DIR_MAX, which is what bounds an installed app's directory name --
 * an icon this cannot spell belongs to an app that cannot be installed. */
#define ND_THEME_ICON_NAME_MAX 48

#define UI_RES_DIR "/NeoDCT/System/ui/resources/"

bool nd_theme_resource(const char *system_path, char *out, size_t out_sz)
{
    const nd_theme_info *t = nd_theme_active();
    char rel[ND_PATH_MAX];
    char cand[ND_PATH_MAX];
    const char *rest;
    char app[ND_THEME_ICON_NAME_MAX];

    if (system_path == NULL || out == NULL || out_sz == 0u)
        return false;
    /* Always leave a usable path behind, so a caller that ignores the return
     * value is still correct. Every early return below has already done this. */
    (void)nd_strlcpy(out, system_path, out_sz);

    if (t->dir[0] == '\0')
        return false;

    rel[0] = '\0';
    rest = after_prefix(system_path, UI_RES_DIR "fonts/");
    if (rest != NULL) {
        if (strcmp(rest, "aero.ttf") == 0) {
            if (!t->has_font)
                return false;
            (void)nd_strlcpy(rel, "fonts/ui.ttf", sizeof rel);
        } else if (strcmp(rest, "aero-bold.ttf") == 0) {
            if (!t->has_font_bold)
                return false;
            (void)nd_strlcpy(rel, "fonts/ui-bold.ttf", sizeof rel);
        } else {
            /* font.ttf, the pixel face. Deliberately NOT themeable: the boot
             * bar's glyph tables are baked out of it and it is pinned by
             * sha256 in the golden set. See nd_paths.h. */
            return false;
        }
    } else if ((rest = after_prefix(system_path, UI_RES_DIR "img/")) != NULL) {
        if (!t->has_img)
            return false;
        if (nd_snprintf(rel, sizeof rel, "img/%s", rest) != ND_OK)
            return false;
    } else if (app_icon_name(system_path, app, sizeof app)) {
        if (!t->has_icons)
            return false;
        if (nd_snprintf(rel, sizeof rel, "icons/%s.png", app) != ND_OK)
            return false;
    } else {
        return false;
    }

    if (join_virtual(cand, sizeof cand, t->dir, rel) != ND_OK)
        return false;
    /* The theme said it has an icons/ directory; it did not promise an icon
     * for THIS app. The per-file test is what makes a partial theme legal --
     * and a partial theme is the normal case, since nobody draws seventeen
     * icons to change a colour. */
    if (!nd_path_is_file(cand))
        return false;

    (void)nd_strlcpy(out, cand, out_sz);
    return true;
}

bool nd_theme_wallpaper(char *out, size_t out_sz)
{
    const nd_theme_info *t = nd_theme_active();

    if (out == NULL || out_sz == 0u)
        return false;
    out[0] = '\0';
    if (t->dir[0] == '\0' || !t->has_wallpaper)
        return false;
    if (join_virtual(out, out_sz, t->dir, "wallpaper.jpg") != ND_OK) {
        out[0] = '\0';
        return false;
    }
    return true;
}

bool nd_theme_preview_path(const nd_theme_info *t, char *out, size_t out_sz)
{
    if (t == NULL || out == NULL || out_sz == 0u)
        return false;
    out[0] = '\0';
    if (t->dir[0] == '\0' || !t->has_preview)
        return false;
    if (join_virtual(out, out_sz, t->dir, ND_THEME_PREVIEW) != ND_OK) {
        out[0] = '\0';
        return false;
    }
    return true;
}
