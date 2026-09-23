/* test_phonebook.c -- the PhoneBook app and the shared contact picker.
 *
 * Two things are under test and they live in two different binaries:
 *
 *   lib/nd_contacts.c        is inside libneodct, which this test already
 *                            links, so it is called directly.
 *   apps/PhoneBook/app.so    is a separate artefact. It is dlopen()ed rather
 *                            than recompiled into this binary, for the reason
 *                            test_cubebench.c gives: recompiling main.c here
 *                            would test a second copy built with different
 *                            flags instead of the file that ships.
 *
 * ============ WHAT IT CLAIMS ============
 *
 *  1. TWO GOLDEN FRAMES, drawn by the code that draws them on the phone.
 *     app-phonebook is app_run() itself, driven to its first screen and let
 *     out with Back; contacts-picker is nd_contacts_show_selector() over the
 *     seeded "NeoDCT Support" row. Both are compared by the SHA-256 over raw
 *     RGB that goldenframe.py compares, so a pass here is a pass there.
 *     spec-build-test.md section 3.6 records app-phonebook as byte-identical
 *     to widget-verticallist -- these two are drawn by different code, so
 *     agreeing is a real check rather than a tautology.
 *
 *  2. THE PICKER'S THREE OUTCOMES: a row chosen (with its index, which is
 *     what the "1-1-<n>" breadcrumb is built from), Back, and the empty
 *     state -- whose two different words, "No Results" and "No Contacts", are
 *     chosen by whether a query was passed and not by whether it matched.
 *
 *  3. THE EMPTY STATE CLEARS ONLY ROWS 0..145. Every widget in the framework
 *     leaves the softkey strip alone and the picker's hand-drawn screen is no
 *     exception; the canvas is pre-filled with grey so a full-height clear
 *     would be visible.
 *
 *  4. THE QUERY IS list_ui's: ORDER BY name ASC, LIKE with a wildcard on both
 *     sides, and sqlite's ASCII-case-insensitive LIKE.
 *
 *  5. THE THREE STATEMENTS round-trip against a real sqlite database, and the
 *     rows they write are the ones the picker then reads.
 *
 *  6. THE CALLING SCREEN IS THE BUG. Its three lines land at (10,43),
 *     (10,78) and (10,103) and nothing else happens -- no dial, no call
 *     screen. The frame is compared against one composed by hand from those
 *     three coordinates, so "it stopped drawing the number" and "it started
 *     dialling" are both failures here.
 */

#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <linux/input.h>

#include "nd_app.h"
#include "nd_capture.h"
#include "nd_contacts.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_image.h"
#include "nd_input.h"
#include "nd_json.h"
#include "nd_keycodes.h"
#include "nd_keypad.h"
#include "nd_paths.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "../../apps/PhoneBook/phonebook.h"
#include "platform_test.h"

#define FONT_REL "overlay/NeoDCT/System/ui/resources/fonts/font.ttf"

/* ------------------------------------------------------------------ *
 * Finding the font, the reference set and the built app.so
 * ------------------------------------------------------------------ */

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");

    if (f == NULL)
        return false;
    (void)fclose(f);
    return true;
}

static bool resolve_golden(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_GOLDEN");

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, sz, "%.900s", env);
        return true;
    }
    if (file_exists("../tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "../tests/golden", sz);
        return true;
    }
    if (file_exists("neodct/tests/golden/manifest.json")) {
        (void)nd_strlcpy(out, "neodct/tests/golden", sz);
        return true;
    }
    return false;
}

/* font.ttf is never under NEODCT_ROOT, so it is opened with plain fopen. */
static bool resolve_font(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_FONT");
    char golden[1024];
    char base[512];
    char cand[1024];
    char *cut;

    if (env != NULL && env[0] != '\0') {
        (void)snprintf(out, sz, "%.900s", env);
        return true;
    }
    if (resolve_golden(golden, sizeof golden)) {
        (void)snprintf(base, sizeof base, "%.480s", golden);
        cut = strrchr(base, '/');
        if (cut != NULL)
            *cut = '\0';
        cut = strrchr(base, '/');
        if (cut != NULL)
            *cut = '\0';
        (void)snprintf(cand, sizeof cand, "%.400s/" FONT_REL, base);
        if (file_exists(cand)) {
            (void)nd_strlcpy(out, cand, sz);
            return true;
        }
    }
    if (file_exists("../" FONT_REL)) {
        (void)nd_strlcpy(out, "../" FONT_REL, sz);
        return true;
    }
    if (file_exists("neodct/" FONT_REL)) {
        (void)nd_strlcpy(out, "neodct/" FONT_REL, sz);
        return true;
    }
    if (file_exists(ND_PATH_FONT)) {
        (void)nd_strlcpy(out, ND_PATH_FONT, sz);
        return true;
    }
    return false;
}

/* build/<variant>/test/test_phonebook -> build/<variant>/apps/PhoneBook/app.so,
 * so an ASan run loads the ASan app and never a stale default-variant one. */
static bool resolve_app_so(char *out, size_t sz)
{
    const char *env = getenv("NEODCT_PHONEBOOK_SO");
    char exe[ND_PATH_MAX];
    ssize_t n;
    char *slash;

    if (env != NULL && env[0] != '\0') {
        (void)nd_strlcpy(out, env, sz);
        return true;
    }
    n = readlink("/proc/self/exe", exe, sizeof exe - 1u);
    if (n <= 0)
        return false;
    exe[n] = '\0';
    slash = strrchr(exe, '/');
    if (slash == NULL)
        return false;
    *slash = '\0';
    return nd_snprintf(out, sz, "%s/../apps/PhoneBook/app.so", exe) == ND_OK;
}

/* ------------------------------------------------------------------ *
 * The app's exported surface
 * ------------------------------------------------------------------ */

typedef struct {
    void *handle;
    int (*run)(nd_ui *);
    void (*shutdown)(void);
    nd_err (*insert)(const char *, const char *, char *, size_t);
    nd_err (*update)(int64_t, const char *, const char *, char *, size_t);
    nd_err (*del)(int64_t, char *, size_t);
    void (*center_message)(nd_ui *, const char *, double, const nd_font *, nd_color);
    void (*calling_screen)(nd_ui *, const nd_contact *);
    nd_err (*options_root)(char *, size_t, size_t);
    const char *const *main_items;
    const char *const *opt_items;
    const char *const *contact_items;
} pb_api;

static pb_api g_api;
static char g_so[ND_PATH_MAX];

static void *sym(void *h, const char *name)
{
    void *p = dlsym(h, name);

    if (p == NULL)
        fprintf(stderr, "test_phonebook: app.so has no symbol %s\n", name);
    return p;
}

static bool api_open(void)
{
    void *h;

    if (!resolve_app_so(g_so, sizeof g_so))
        return false;
    h = dlopen(g_so, RTLD_NOW | RTLD_LOCAL);
    if (h == NULL) {
        fprintf(stderr, "test_phonebook: dlopen %s: %s\n", g_so, dlerror());
        return false;
    }
    g_api.handle = h;

    *(void **)&g_api.run = sym(h, "app_run");
    *(void **)&g_api.shutdown = sym(h, "app_shutdown");
    *(void **)&g_api.insert = sym(h, "nd_phonebook_insert");
    *(void **)&g_api.update = sym(h, "nd_phonebook_update");
    *(void **)&g_api.del = sym(h, "nd_phonebook_delete");
    *(void **)&g_api.center_message = sym(h, "nd_phonebook_center_message");
    *(void **)&g_api.calling_screen = sym(h, "nd_phonebook_calling_screen");
    *(void **)&g_api.options_root = sym(h, "nd_phonebook_options_root");
    g_api.main_items = dlsym(h, "nd_phonebook_main_items");
    g_api.opt_items = dlsym(h, "nd_phonebook_opt_items");
    g_api.contact_items = dlsym(h, "nd_phonebook_contact_items");

    return g_api.run != NULL && g_api.shutdown != NULL && g_api.insert != NULL &&
           g_api.update != NULL && g_api.del != NULL && g_api.center_message != NULL &&
           g_api.calling_screen != NULL && g_api.options_root != NULL && g_api.main_items != NULL &&
           g_api.opt_items != NULL && g_api.contact_items != NULL;
}

static void api_close(void)
{
    if (g_api.handle != NULL)
        (void)dlclose(g_api.handle);
    memset(&g_api, 0, sizeof g_api);
}

/* ------------------------------------------------------------------ *
 * A UI context with nothing behind it but memory
 * ------------------------------------------------------------------ */

typedef struct {
    nd_ui ui;
    nd_draw draw;
    nd_image *canvas;
    nd_font *font_s;
    nd_font *font_md;
    nd_font *font_n;
    nd_font *font_xl;
    nd_input *input;
    int write_fd;
} fixture;

/* ============ WHY THE FIXTURE HAS A WALLPAPER ============
 *
 * The reference frames this test compares against come out of nd-shoot, whose
 * app group runs every stock app with Palestine.jpg set. Since the framework
 * started drawing the wallpaper behind app chrome, that is what those frames
 * contain, and a fixture that rendered on black would differ from them in
 * three quarters of its pixels -- in the background, not in anything PhoneBook
 * did.
 *
 * So the fixture establishes the same two facts nd_ui_init_app() does for a
 * real app process: manifest.json's "useWallpaper", read from the app's OWN
 * manifest rather than assumed here, and the wallpaper itself. ND_ROOT points
 * at the overlay for the two reads and is put back afterwards.
 *
 * `font_path` is <overlay>/NeoDCT/System/ui/resources/fonts/font.ttf, which is
 * where the overlay root is recovered from -- resolve_font() has already done
 * the searching. */
static void fx_apply_reference_wallpaper(fixture *fx, const char *font_path)
{
    static const char *const FONT_TAIL = "/NeoDCT/System/ui/resources/fonts/font.ttf";
    char overlay[1024];
    char saved[ND_PATH_MAX];
    size_t flen = strlen(font_path);
    size_t tlen = strlen(FONT_TAIL);

    fx->ui.app_use_wallpaper = false;
    if (flen <= tlen || strcmp(font_path + (flen - tlen), FONT_TAIL) != 0)
        return;
    if (flen - tlen >= sizeof overlay)
        return;
    memcpy(overlay, font_path, flen - tlen);
    overlay[flen - tlen] = '\0';

    (void)nd_strlcpy(saved, nd_path_root(), sizeof saved);
    if (nd_path_set_root(overlay) != ND_OK)
        return;
    fx->ui.app_use_wallpaper = nd_app_manifest_use_wallpaper("/NeoDCT/System/apps/PhoneBook");
    if (fx->ui.app_use_wallpaper)
        nd_ui_set_wallpaper(&fx->ui,
                            nd_ui_load_wallpaper("/NeoDCT/System/wallpapers/Palestine.jpg"));
    (void)nd_path_set_root(saved[0] != '\0' ? saved : NULL);
}

static bool fx_init(fixture *fx)
{
    char path[1024];

    memset(fx, 0, sizeof *fx);
    fx->write_fd = -1;
    if (!resolve_font(path, sizeof path)) {
        fprintf(stderr, "test_phonebook: cannot find font.ttf; set NEODCT_FONT\n");
        return false;
    }
    fx->font_s = nd_font_load(path, ND_FONT_PX_S);
    fx->font_md = nd_font_load(path, ND_FONT_PX_MD);
    fx->font_n = nd_font_load(path, ND_FONT_PX_N);
    fx->font_xl = nd_font_load(path, ND_FONT_PX_XL);
    if (fx->font_s == NULL || fx->font_md == NULL || fx->font_n == NULL || fx->font_xl == NULL) {
        fprintf(stderr, "test_phonebook: nd_font_load(%s) failed\n", path);
        return false;
    }

    /* 240 * 175 * 3 = 126,000 bytes -- one UI frame. */
    fx->canvas = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_BLACK);
    if (fx->canvas == NULL)
        return false;
    if (nd_draw_bind(&fx->draw, fx->canvas) != ND_OK)
        return false;

    fx->ui.w = ND_UI_W;
    fx->ui.h = ND_UI_H;
    fx->ui.softkey_h = ND_SOFTKEY_H;
    fx->ui.content_bottom = ND_UI_H - ND_SOFTKEY_H;
    fx->ui.canvas = fx->canvas;
    fx->ui.draw = &fx->draw;
    fx->ui.fb = NULL; /* no panel: the canvas is the frame */
    fx->ui.font_s = fx->font_s;
    fx->ui.font_md = fx->font_md;
    fx->ui.font_n = fx->font_n;
    fx->ui.font_xl = fx->font_xl;
    fx->ui.keypad_fd = -1;
    /* Only the core's own bar is transparent, and this context is not it. */
    fx->ui.softkey_exists = true;
    fx_apply_reference_wallpaper(fx, path);
    return true;
}

static void fx_free(fixture *fx)
{
    /* The wallpaper and the chrome copy the fixture caused to be loaded. A
     * real app process ends and the kernel takes them back; a test process
     * runs sixty fixtures and LeakSanitizer counts every one. */
    nd_ui_invalidate_chrome(&fx->ui);
    nd_ui_set_wallpaper(&fx->ui, NULL);
    if (fx->write_fd >= 0)
        (void)close(fx->write_fd);
    if (fx->input != NULL)
        nd_input_close(fx->input);
    nd_image_free(fx->canvas);
    nd_font_free(fx->font_s);
    nd_font_free(fx->font_md);
    nd_font_free(fx->font_n);
    nd_font_free(fx->font_xl);
    memset(fx, 0, sizeof *fx);
}

/* A key channel with the whole script written in advance, so no widget loop
 * can block. Nothing repeats: every press is released. */
static bool fx_keys(fixture *fx)
{
    int fds[2];

    if (fx->input != NULL)
        return true;
    if (pipe(fds) != 0)
        return false;
    if (nd_input_open_fd(&fx->input, fds[0]) != ND_OK) {
        (void)close(fds[0]);
        (void)close(fds[1]);
        return false;
    }
    nd_input_set_repeat(fx->input, 0.0, 0.0);
    fx->write_fd = fds[1];
    fx->ui.input = fx->input;
    return true;
}

static void write_key(int fd, uint16_t code, int32_t value)
{
    struct input_event ev;

    memset(&ev, 0, sizeof ev);
    ev.type = EV_KEY;
    ev.code = code;
    ev.value = value;
    if (write(fd, &ev, sizeof ev) != (ssize_t)sizeof ev)
        CHECK(false);
}

static void script_key(fixture *fx, int32_t code)
{
    write_key(fx->write_fd, (uint16_t)code, 1);
    write_key(fx->write_fd, (uint16_t)code, 0);
}

/* ------------------------------------------------------------------ *
 * The golden manifest
 * ------------------------------------------------------------------ */

static nd_json_doc *g_manifest;
static const nd_json_val *g_frames;

static void manifest_open(void)
{
    char dir[1024];
    char path[1200];
    uint8_t *buf = NULL;
    long len;
    FILE *f;
    char err[128];
    const nd_json_val *root;

    if (!resolve_golden(dir, sizeof dir))
        return;
    (void)snprintf(path, sizeof path, "%.1000s/manifest.json", dir);

    /* Plain fopen: the reference set is not under NEODCT_ROOT. */
    f = fopen(path, "rb");
    if (f == NULL)
        return;
    if (fseek(f, 0, SEEK_END) != 0)
        goto done;
    len = ftell(f);
    if (len <= 0 || fseek(f, 0, SEEK_SET) != 0)
        goto done;
    buf = malloc((size_t)len);
    if (buf == NULL)
        goto done;
    if (fread(buf, 1u, (size_t)len, f) != (size_t)len)
        goto done;
    if (nd_json_parse(buf, (size_t)len, &g_manifest, err, sizeof err) != ND_OK) {
        fprintf(stderr, "test_phonebook: manifest parse: %s\n", err);
        g_manifest = NULL;
        goto done;
    }
    root = nd_json_root(g_manifest);
    g_frames = nd_json_get(root, "frames");
done:
    free(buf);
    (void)fclose(f);
}

static const char *golden_sha(const char *name)
{
    size_t i;

    if (g_frames == NULL)
        return NULL;
    for (i = 0u; i < nd_json_len(g_frames); i++) {
        const nd_json_val *fr = nd_json_at(g_frames, i);

        if (strcmp(nd_json_get_str(fr, "name", ""), name) == 0)
            return nd_json_get_str(fr, "sha256", NULL);
    }
    return NULL;
}

static void check_frame(const nd_image *img, const char *name)
{
    char got[65];
    const char *want = golden_sha(name);

    if (nd_capture_digest(img, got, sizeof got) != ND_OK) {
        CHECK(false);
        return;
    }
    if (want == NULL) {
        fprintf(stderr, "test_phonebook: no reference for %s (got %s)\n", name, got);
        CHECK(false);
        return;
    }
    g_checks++;
    if (strcmp(got, want) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL frame %s\n  got  %s\n  want %s\n", name, got, want);
    }
}

/* ------------------------------------------------------------------ *
 * Database helpers
 * ------------------------------------------------------------------ */

/* init_databases(): creates /NeoDCT/User/db under the per-case scratch root
 * and seeds ("NeoDCT Support", "555-1234", 2) when contacts is empty. */
static void db_init(void)
{
    CHECK_INT(nd_db_init_all(), ND_OK);
}

static size_t all_contacts(nd_contact *out, size_t max)
{
    return nd_contacts_query(NULL, out, max);
}

/* ------------------------------------------------------------------ *
 * 1. The two golden frames
 * ------------------------------------------------------------------ */

/* app_run() drawn to its first screen. The Python's capture recipe passes
 * keys=[] and lets ScriptExhausted out of the first read_keypress; Back
 * reaches the same place, because the frame that is already on the canvas
 * when the key is read is the one being compared. */
static void test_golden_app_phonebook(void)
{
    fixture fx;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();

    script_key(&fx, ND_KEY_CLEAR);
    CHECK_INT(g_api.run(&fx.ui), 0);
    check_frame(fx.canvas, "app-phonebook");

    fx_free(&fx);
}

/* shoot_telephony's last act: show_contact_selector(ui, "Select", "Call")
 * over the one seeded row, left through Back. */
static void test_golden_contacts_picker(void)
{
    fixture fx;
    nd_contact picked;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();

    memset(&picked, 0, sizeof picked);
    script_key(&fx, ND_KEY_CLEAR);
    CHECK(!nd_contacts_show_selector(&fx.ui, "Select", "Call", &picked));
    check_frame(fx.canvas, "contacts-picker");

    /* Back leaves *out untouched -- the Python returns None and the caller
     * never unpacks it. */
    CHECK_STR(picked.name, "");

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 2. The picker's three outcomes
 * ------------------------------------------------------------------ */

static void test_picker_returns_row_and_index(void)
{
    fixture fx;
    nd_contact got;
    size_t idx = 99u;
    char err[ND_PHONEBOOK_ERR_MAX];

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();
    CHECK_INT(g_api.insert("Alice", "111", err, sizeof err), ND_OK);
    CHECK_INT(g_api.insert("Zoe", "999", err, sizeof err), ND_OK);

    /* ORDER BY name ASC over {Alice, NeoDCT Support, Zoe}: Down once lands on
     * "NeoDCT Support", which is row 1. */
    script_key(&fx, ND_KEY_DOWN);
    script_key(&fx, ND_KEY_ENTER);
    memset(&got, 0, sizeof got);
    CHECK(nd_contacts_pick(&fx.ui, "Edit", "Edit", NULL, "1-3", &got, &idx));
    CHECK_STR(got.name, "NeoDCT Support");
    CHECK_STR(got.number, "555-1234");
    CHECK_INT(got.speed_dial, 2);
    CHECK_INT(idx, 1);

    /* out_index is optional: the core's wrapper passes NULL for it. */
    script_key(&fx, ND_KEY_ENTER);
    memset(&got, 0, sizeof got);
    CHECK(nd_contacts_pick(&fx.ui, NULL, NULL, NULL, NULL, &got, NULL));
    CHECK_STR(got.name, "Alice");

    /* A digit shortcut is VerticalList's, and it reaches the picker: '3'
     * picks the third row without moving the cursor first. */
    script_key(&fx, ND_KEY_3);
    memset(&got, 0, sizeof got);
    idx = 99u;
    CHECK(nd_contacts_pick(&fx.ui, "Erase", "Erase", NULL, "1-4", &got, &idx));
    CHECK_STR(got.name, "Zoe");
    CHECK_INT(idx, 2);

    fx_free(&fx);
}

static void test_picker_back(void)
{
    fixture fx;
    nd_contact got;
    size_t idx = 7u;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();

    script_key(&fx, ND_KEY_CLEAR);
    memset(&got, 0, sizeof got);
    CHECK(!nd_contacts_pick(&fx.ui, "Select", "Call", NULL, "1", &got, &idx));
    CHECK_INT(idx, 7); /* untouched */

    fx_free(&fx);
}

/* The empty state: two different words, chosen by whether a query was PASSED
 * and not by whether it matched anything. Both are centred with font_n, and
 * both leave rows 146..174 alone. */
/* What the framework paints where a widget clears rows 0..content_bottom.
 * It used to be a flat black fill; with the wallpaper drawn behind app chrome
 * it is the wallpaper's own rows 0..content_bottom, and a hand-built
 * expectation has to say the same thing or it is testing the old design. */
static void expect_content_background(fixture *fx, nd_image *expect, nd_draw *d)
{
    const nd_image *bg = nd_ui_chrome_wallpaper(&fx->ui);

    if (bg != NULL)
        (void)nd_image_blit_region(expect, bg, ND_RECT(0, 0, ND_UI_W, ND_UI_H - ND_SOFTKEY_H), 0,
                                   0);
    else
        (void)nd_draw_rect_fill(d, ND_RECT(0, 0, ND_UI_W, ND_UI_H - ND_SOFTKEY_H), ND_BLACK);
}

static void check_empty_screen(fixture *fx, const char *msg)
{
    nd_image *expect;
    nd_draw d;
    int32_t w = 0;
    int32_t h = 0;
    char a[65];
    char b[65];

    expect = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_GRAY);
    if (expect == NULL || nd_draw_bind(&d, expect) != ND_OK) {
        CHECK(false);
        nd_image_free(expect);
        return;
    }
    expect_content_background(fx, expect, &d);
    nd_text_size(fx->ui.font_n, msg, &w, &h);
    (void)nd_draw_text(&d, (ND_UI_W - w) / 2, nd_max32(10, (ND_UI_H - ND_SOFTKEY_H - h) / 2), msg,
                       fx->ui.font_n, ND_WHITE);

    CHECK_INT(nd_capture_digest(fx->canvas, a, sizeof a), ND_OK);
    CHECK_INT(nd_capture_digest(expect, b, sizeof b), ND_OK);
    g_checks++;
    if (strcmp(a, b) != 0) {
        g_failures++;
        fprintf(stderr, "FAIL empty screen \"%s\"\n  got  %s\n  want %s\n", msg, a, b);
    }
    nd_image_free(expect);
}

static void test_picker_empty_states(void)
{
    fixture fx;
    nd_contact got;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    /* No database at all: the Python's sqlite3.connect() would create an
     * empty file and the SELECT would then raise. nd_contacts_query returns
     * no rows, which lands here -- OPEN-QUESTIONS.md PB-4. */
    (void)nd_draw_rect_fill(&fx.draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_GRAY);
    memset(&got, 0, sizeof got);
    CHECK(!nd_contacts_pick(&fx.ui, "Select", "Call", NULL, "1", &got, NULL));
    check_empty_screen(&fx, "No Contacts");

    /* An empty query string is falsy in Python, so it is NOT a search. */
    (void)nd_draw_rect_fill(&fx.draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_GRAY);
    CHECK(!nd_contacts_pick(&fx.ui, "Select", "Call", "", "1", &got, NULL));
    check_empty_screen(&fx, "No Contacts");

    /* A seeded database plus a query that matches nothing. */
    db_init();
    (void)nd_draw_rect_fill(&fx.draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_GRAY);
    CHECK(!nd_contacts_pick(&fx.ui, "Results", "Options", "zzzz", "1-1", &got, NULL));
    check_empty_screen(&fx, "No Results");

    fx_free(&fx);
}

/* The picker reads at most ND_CONTACTS_PICK_MAX rows. The heap arrays are
 * that size, so this is also the case ASan would catch an overflow in. */
static void test_picker_caps_the_list(void)
{
    fixture fx;
    nd_contact got;
    nd_contact *rows;
    char name[32];
    char err[ND_PHONEBOOK_ERR_MAX];
    size_t i;
    size_t n;
    const size_t OVER = ND_CONTACTS_PICK_MAX + 8u;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();
    for (i = 0u; i < OVER; i++) {
        (void)nd_snprintf(name, sizeof name, "A%04zu", i);
        CHECK_INT(g_api.insert(name, "0", err, sizeof err), ND_OK);
    }

    rows = malloc(OVER * sizeof *rows);
    if (rows == NULL) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    /* OVER inserted plus the seeded row -- more than the picker's array
     * holds, and more than nd_contacts_query() will write. */
    n = all_contacts(rows, OVER);
    CHECK_INT(n, OVER);
    n = all_contacts(rows, ND_CONTACTS_PICK_MAX);
    CHECK_INT(n, ND_CONTACTS_PICK_MAX);
    free(rows);

    /* Row 0 is "A0000": every inserted name sorts before "NeoDCT Support". */
    script_key(&fx, ND_KEY_ENTER);
    memset(&got, 0, sizeof got);
    CHECK(nd_contacts_pick(&fx.ui, "Select", "Call", NULL, "1", &got, NULL));
    CHECK_STR(got.name, "A0000");

    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 3. The query
 * ------------------------------------------------------------------ */

static void test_query_shape(void)
{
    nd_contact rows[8];
    char err[ND_PHONEBOOK_ERR_MAX];
    size_t n;

    db_init();
    CHECK_INT(g_api.insert("Bob", "1", err, sizeof err), ND_OK);
    CHECK_INT(g_api.insert("Jimbo", "2", err, sizeof err), ND_OK);
    CHECK_INT(g_api.insert("Robot", "3", err, sizeof err), ND_OK);
    CHECK_INT(g_api.insert("Alice", "4", err, sizeof err), ND_OK);

    /* ORDER BY name ASC. */
    n = all_contacts(rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 5);
    CHECK_STR(rows[0].name, "Alice");
    CHECK_STR(rows[1].name, "Bob");
    CHECK_STR(rows[2].name, "Jimbo");
    CHECK_STR(rows[3].name, "NeoDCT Support");
    CHECK_STR(rows[4].name, "Robot");

    /* '%' || q || '%' -- list_ui's own comment: "bo" matches Bob, Jimbo,
     * Robot. sqlite's LIKE is ASCII-case-insensitive by default. */
    n = nd_contacts_query("bo", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 3);
    CHECK_STR(rows[0].name, "Bob");
    CHECK_STR(rows[1].name, "Jimbo");
    CHECK_STR(rows[2].name, "Robot");

    n = nd_contacts_query("BO", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 3);

    n = nd_contacts_query("nothing here", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 0);
}

/* ------------------------------------------------------------------ *
 * 4. The three statements
 * ------------------------------------------------------------------ */

static void test_db_actions(void)
{
    nd_contact rows[8];
    char err[ND_PHONEBOOK_ERR_MAX];
    size_t n;
    int64_t id;

    db_init();

    /* INSERT writes speed_dial 0 -- "1-touch dialing" has no branch behind
     * it, so nothing ever sets it to anything else. */
    err[0] = 'x';
    CHECK_INT(g_api.insert("Mum", "0741234567", err, sizeof err), ND_OK);
    CHECK_STR(err, "");
    n = nd_contacts_query("Mum", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 1);
    CHECK_STR(rows[0].number, "0741234567");
    CHECK_INT(rows[0].speed_dial, 0);
    id = rows[0].id;

    /* An empty name is accepted: edit_contact_action tests `is None`. */
    CHECK_INT(g_api.update(id, "", "", err, sizeof err), ND_OK);
    n = all_contacts(rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 2);
    CHECK_STR(rows[0].name, ""); /* "" sorts before "NeoDCT Support" */
    CHECK_STR(rows[0].number, "");

    CHECK_INT(g_api.update(id, "Mum", "0741234567", err, sizeof err), ND_OK);
    CHECK_INT(g_api.del(id, err, sizeof err), ND_OK);
    n = nd_contacts_query("Mum", rows, ND_ARRAY_LEN(rows));
    CHECK_INT(n, 0);

    /* Deleting a row that is not there is not an error in sqlite, and the
     * Python would show "Erased" for it too. */
    CHECK_INT(g_api.del(id, err, sizeof err), ND_OK);

    /* The err buffer is optional. */
    CHECK_INT(g_api.insert("Dad", "1", NULL, 0u), ND_OK);
    CHECK_INT(g_api.insert(NULL, "1", err, sizeof err), ND_ERR_INVAL);
    CHECK_INT(g_api.update(1, "a", NULL, err, sizeof err), ND_ERR_INVAL);
}

/* ------------------------------------------------------------------ *
 * 5. The menus and the breadcrumb
 * ------------------------------------------------------------------ */

static void test_menu_tables(void)
{
    CHECK_STR(g_api.main_items[0], "Search");
    CHECK_STR(g_api.main_items[1], "Add entry");
    CHECK_STR(g_api.main_items[2], "Edit");
    CHECK_STR(g_api.main_items[3], "Erase");
    CHECK_STR(g_api.main_items[4], "Send entry");
    CHECK_STR(g_api.main_items[5], "Options");
    CHECK_STR(g_api.main_items[6], "1-touch dialing");

    CHECK_STR(g_api.opt_items[0], "Type of view");
    CHECK_STR(g_api.opt_items[1], "Memory status");

    CHECK_STR(g_api.contact_items[0], "Call");
    CHECK_STR(g_api.contact_items[1], "Edit");
    CHECK_STR(g_api.contact_items[2], "Delete");
    CHECK_STR(g_api.contact_items[3], "Send number");
}

static void test_options_root(void)
{
    char buf[16];

    /* "1-1-%d" % (selection_index + 1) */
    CHECK_INT(g_api.options_root(buf, sizeof buf, 0u), ND_OK);
    CHECK_STR(buf, "1-1-1");
    CHECK_INT(g_api.options_root(buf, sizeof buf, 3u), ND_OK);
    CHECK_STR(buf, "1-1-4");
    CHECK_INT(g_api.options_root(buf, sizeof buf, 254u), ND_OK);
    CHECK_STR(buf, "1-1-255");

    /* Truncation is reported, not silently trimmed. */
    CHECK_INT(g_api.options_root(buf, 5u, 9u), ND_ERR_TOOLONG);
    CHECK_INT(g_api.options_root(NULL, sizeof buf, 0u), ND_ERR_INVAL);
}

/* ------------------------------------------------------------------ *
 * 6. The two hand-drawn screens
 * ------------------------------------------------------------------ */

static void test_center_message(void)
{
    fixture fx;
    nd_image *expect;
    nd_draw d;
    int32_t w = 0;
    int32_t h = 0;
    char a[65];
    char b[65];

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    expect = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_GRAY);
    if (expect == NULL || nd_draw_bind(&d, expect) != ND_OK) {
        CHECK(false);
        nd_image_free(expect);
        fx_free(&fx);
        return;
    }

    /* font NULL means font_xl -- the Python's `font = font or ui.font_xl`. */
    (void)nd_draw_rect_fill(&fx.draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_GRAY);
    g_api.center_message(&fx.ui, "Saved!", 0.0, NULL, ND_WHITE);

    expect_content_background(&fx, expect, &d);
    nd_text_size(fx.ui.font_xl, "Saved!", &w, &h);
    (void)nd_draw_text(&d, (ND_UI_W - w) / 2, nd_max32(10, (ND_UI_H - ND_SOFTKEY_H - h) / 2),
                       "Saved!", fx.ui.font_xl, ND_WHITE);

    CHECK_INT(nd_capture_digest(fx.canvas, a, sizeof a), ND_OK);
    CHECK_INT(nd_capture_digest(expect, b, sizeof b), ND_OK);
    CHECK_STR(a, b);

    nd_image_free(expect);
    fx_free(&fx);
}

/* THE BUG, pinned. run_contact_options item 0 paints three lines and does
 * nothing else: no dial, no call screen, no hang-up key. */
static void test_calling_screen_does_not_dial(void)
{
    fixture fx;
    nd_image *expect;
    nd_draw d;
    nd_contact c;
    char a[65];
    char b[65];
    int32_t y;

    if (!fx_init(&fx)) {
        CHECK(false);
        return;
    }
    expect = nd_image_new_filled(ND_UI_W, ND_UI_H, ND_PIXFMT_RGB888, ND_GRAY);
    if (expect == NULL || nd_draw_bind(&d, expect) != ND_OK) {
        CHECK(false);
        nd_image_free(expect);
        fx_free(&fx);
        return;
    }

    memset(&c, 0, sizeof c);
    c.id = 1;
    (void)nd_strlcpy(c.name, "Mum", sizeof c.name);
    (void)nd_strlcpy(c.number, "0741234567", sizeof c.number);

    /* max(12, int(145 * 0.30)) -- int() truncates, so 43 and not 44. */
    y = nd_max32(12, nd_trunc32(145.0 * 0.30));
    CHECK_INT(y, 43);

    (void)nd_draw_rect_fill(&fx.draw, ND_RECT(0, 0, ND_UI_W, ND_UI_H), ND_GRAY);
    /* ui->modem is NULL in this fixture, exactly as it is inside any app
     * process (nd_app.h). A version of this screen that tried to dial would
     * have nothing to dial with -- which is the point: it never tries. */
    CHECK(fx.ui.modem == NULL);
    g_api.calling_screen(&fx.ui, &c);
    CHECK(fx.ui.modem == NULL);

    expect_content_background(&fx, expect, &d);
    (void)nd_draw_text(&d, 10, y, "Calling...", fx.ui.font_xl, ND_WHITE);
    (void)nd_draw_text(&d, 10, y + 35, "Mum", fx.ui.font_n, ND_WHITE);
    (void)nd_draw_text(&d, 10, y + 60, "0741234567", fx.ui.font_s, ND_WHITE);

    CHECK_INT(nd_capture_digest(fx.canvas, a, sizeof a), ND_OK);
    CHECK_INT(nd_capture_digest(expect, b, sizeof b), ND_OK);
    CHECK_STR(a, b);

    nd_image_free(expect);
    fx_free(&fx);
}

/* ------------------------------------------------------------------ *
 * 7. run() walks its menu
 * ------------------------------------------------------------------ */

/* Down to "Options" (index 5), Enter, then Back out of both menus. The two
 * dead entries are proved dead the same way: choosing "Send entry" and
 * "1-touch dialing" leaves the app on its own main menu, so the Back that
 * follows is the one that ends it. */
static void test_run_navigates(void)
{
    fixture fx;
    int32_t i;

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();

    for (i = 0; i < 5; i++)
        script_key(&fx, ND_KEY_DOWN);
    script_key(&fx, ND_KEY_ENTER); /* Options submenu   */
    script_key(&fx, ND_KEY_ENTER); /* "Type of view"    */
    script_key(&fx, ND_KEY_DOWN);
    script_key(&fx, ND_KEY_ENTER); /* "Memory status"   */
    script_key(&fx, ND_KEY_CLEAR); /* leave the submenu */
    script_key(&fx, ND_KEY_5);     /* "Send entry" -- no branch at all  */
    script_key(&fx, ND_KEY_7);     /* "1-touch dialing" -- likewise     */
    script_key(&fx, ND_KEY_CLEAR); /* leave the app     */
    CHECK_INT(g_api.run(&fx.ui), 0);

    /* Nothing above wrote to the database. */
    {
        nd_contact rows[4];

        CHECK_INT(all_contacts(rows, ND_ARRAY_LEN(rows)), 1);
        CHECK_STR(rows[0].name, "NeoDCT Support");
    }

    fx_free(&fx);
}

/* Erase: pick the seeded row off the full list and confirm it is gone. This
 * is the path with NO are-you-sure dialog -- one Enter on the picker and the
 * row is deleted. */
static void test_run_erase_deletes_immediately(void)
{
    fixture fx;
    nd_contact rows[4];

    if (!fx_init(&fx) || !fx_keys(&fx)) {
        CHECK(false);
        fx_free(&fx);
        return;
    }
    db_init();
    CHECK_INT(all_contacts(rows, ND_ARRAY_LEN(rows)), 1);

    script_key(&fx, ND_KEY_4);     /* "Erase" is main item 4 (index 3) */
    script_key(&fx, ND_KEY_ENTER); /* the only row */
    script_key(&fx, ND_KEY_CLEAR); /* leave the app */
    CHECK_INT(g_api.run(&fx.ui), 0);

    CHECK_INT(all_contacts(rows, ND_ARRAY_LEN(rows)), 0);

    fx_free(&fx);
}

static void test_shutdown_is_exported(void)
{
    g_api.shutdown(); /* must not crash, must not draw */
    CHECK(true);
}

/* ------------------------------------------------------------------ */

int main(void)
{
    manifest_open();
    if (!api_open()) {
        fprintf(stderr, "test_phonebook: cannot load %s\n", g_so);
        api_close();
        return 1;
    }

    RUN(test_golden_app_phonebook);
    RUN(test_golden_contacts_picker);
    RUN(test_picker_returns_row_and_index);
    RUN(test_picker_back);
    RUN(test_picker_empty_states);
    RUN(test_picker_caps_the_list);
    RUN(test_query_shape);
    RUN(test_db_actions);
    RUN(test_menu_tables);
    RUN(test_options_root);
    RUN(test_center_message);
    RUN(test_calling_screen_does_not_dial);
    RUN(test_run_navigates);
    RUN(test_run_erase_deletes_immediately);
    RUN(test_shutdown_is_exported);

    api_close();
    if (g_manifest != NULL)
        nd_json_free(g_manifest);
    pt_cleanup();

    printf("test_phonebook: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
