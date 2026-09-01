/* test_appreg.c -- the app registry against manifests nobody shipped, and the
 * scrollbar at every index the shipped registry can produce.
 *
 * test_appsel.c already pins the nine golden frames and the twenty-two
 * shipped manifests. This file covers the two things that file cannot:
 *
 * 1. THE MANIFESTS THAT DO NOT EXIST YET. `manifest.json` is user-supplied
 *    data -- an app can be side-loaded, and the update system writes these
 *    files -- so every default and every rejection branch in
 *    `_scan_apps_from_dir` (main.py:652) is reachable in production and none
 *    of them is exercised by the shipped set, which is twenty-two files that
 *    all spell every key. A synthetic app tree under its own ND_ROOT drives
 *    them: the id default of 999, the name default of the folder name, the
 *    icon default of "icon.png", the exec default, and the six ways an entry
 *    is dropped (unparseable id, a decimal-point id, malformed JSON, an array
 *    root, a scalar root, no manifest at all) plus the one place this port is
 *    stricter than the Python (a JSON float id -- see A-3).
 *
 * 2. THE SCROLLBAR AT ALL TWENTY-FOUR INDICES. The nine golden frames visit
 *    eight distinct indices. The notch is `track_top + index * (99/23)`
 *    truncated at both corners, and the step's fractional part is
 *    `(7*index mod 23)/23`, so round() and trunc() disagree at eleven of the
 *    twenty-four -- 2, 3, 5, 6, 9, 12, 13, 15, 16, 19, 22 -- of which the
 *    frames visit only 3, 5 and 9. The other eight are places the wrong
 *    rounding rule is invisible to the oracle. Every index is rendered here
 *    and the notch's real bounding box is measured out of the pixels, against
 *    arithmetic done in long double so it cannot be the same expression as the
 *    code under test.
 *
 * Both halves need a root of their own, so the synthetic half runs first and
 * releases it before the overlay half stages the real one. nd_path_set_root()
 * is the only way to move it; nd_paths.h caches the environment variable.
 *
 * Runs with no arguments. NEODCT_GOLDEN names the reference set (the Makefile
 * passes it) and the overlay is found relative to it.
 */

#include <errno.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "nd_capture.h"
#include "nd_fb.h"
#include "nd_image.h"
#include "nd_paths.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_ui_sim.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

static int g_checks;
static int g_failures;

#define CHECK(cond, what)                                                    \
    do {                                                                     \
        g_checks++;                                                          \
        if (!(cond)) {                                                       \
            g_failures++;                                                    \
            fprintf(stderr, "FAIL %s:%d  %s\n", __FILE__, __LINE__, (what)); \
        }                                                                    \
    } while (0)

#define CHECK_INT(got, want, what)                                                              \
    do {                                                                                        \
        long long g_ = (long long)(got);                                                        \
        long long w_ = (long long)(want);                                                       \
        g_checks++;                                                                             \
        if (g_ != w_) {                                                                         \
            g_failures++;                                                                       \
            fprintf(stderr, "FAIL %s:%d  %s: got %lld want %lld\n", __FILE__, __LINE__, (what), \
                    g_, w_);                                                                    \
        }                                                                                       \
    } while (0)

#define CHECK_STR(got, want, what)                                                          \
    do {                                                                                    \
        const char *g_ = (got);                                                             \
        const char *w_ = (want);                                                            \
        g_checks++;                                                                         \
        if (g_ == NULL || strcmp(g_, w_) != 0) {                                            \
            g_failures++;                                                                   \
            fprintf(stderr, "FAIL %s:%d  %s: got \"%s\" want \"%s\"\n", __FILE__, __LINE__, \
                    (what), g_ != NULL ? g_ : "(null)", w_);                                \
        }                                                                                   \
    } while (0)

/* ------------------------------------------------------------------ *
 * Temporary roots
 * ------------------------------------------------------------------ */

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    ND_UNUSED(st);
    ND_UNUSED(flag);
    ND_UNUSED(ftw);
    return remove(path);
}

static bool make_temp_dir(char *out, size_t out_sz)
{
    char tmpl[ND_PATH_MAX];
    const char *base = getenv("TMPDIR");

    if (base == NULL || base[0] == '\0')
        base = "/tmp";
    if (nd_snprintf(tmpl, sizeof tmpl, "%s/ndappreg-XXXXXX", base) != ND_OK)
        return false;
    if (mkdtemp(tmpl) == NULL)
        return false;
    return nd_strlcpy(out, tmpl, out_sz) < out_sz;
}

/* ------------------------------------------------------------------ *
 * PART 1 -- manifests nobody shipped
 * ------------------------------------------------------------------ */

/* Every entry becomes <root>/NeoDCT/System/apps/<folder>/. A NULL manifest
 * means the folder exists with no manifest.json at all, which is how a
 * half-installed app looks on disk. */
static const struct {
    const char *folder;
    const char *manifest;
} SYNTHETIC[] = {
    /* Every key spelled, and an icon that is not the default. */
    {"Alpha", "{\"name\": \"Alpha App\", \"id\": \"5\", \"icon\": \"art/big.png\","
              " \"exec\": \"app.so\"}"},

    /* A JSON NUMBER rather than the shipped string form. int() takes both. */
    {"Beta", "{\"id\": 7}"},

    /* ============ THE MANIFEST IS THE APP'S OWN FILE ============
     *
     * Under /NeoDCT/User/sdcard/apps that means it is a file an attacker
     * wrote, and "icon" was joined to the app directory with no containment
     * at all -- so it named any path on the phone, and the CORE opened it,
     * as ndusr, on the menu draw, with no app launched at all. The decoders
     * are the interesting target; a failed decode is the good case.
     *
     * All three keep their app -- a bad icon is not a bad app -- and fall
     * back to the default name, which is what a missing icon already did. */
    {"Climber", "{\"name\": \"Climber\", \"id\": \"31\","
                " \"icon\": \"../../../../etc/shadow\"}"},
    {"Absolute", "{\"name\": \"Absolute\", \"id\": \"32\","
                 " \"icon\": \"/NeoDCT/User/.remote/id_ed25519\"}"},
    {"Sneaky", "{\"name\": \"Sneaky\", \"id\": \"33\","
               " \"icon\": \"art/../../../NeoDCT/User/db/contacts.db\"}"},

    /* No id at all -> data.get("id", 999). */
    {"NoId", "{\"name\": \"No Id\"}"},

    /* int(" 12 ") is 12 in Python: str.strip() is implicit. */
    {"Padded", "{\"name\": \"Padded\", \"id\": \"  12  \"}"},

    /* A negative id is legal and sorts first. Nothing forbids it and the
     * update system does not validate the field. */
    {"Negative", "{\"name\": \"Negative\", \"id\": \"-5\"}"},

    /* int("abc") raises inside the try, so the WHOLE app is dropped -- it
     * does not fall back to 999. */
    {"BadId", "{\"name\": \"Bad Id\", \"id\": \"abc\"}"},

    /* int("7.5") raises too: Python's int() refuses a decimal point in a
     * string, unlike float(). Also dropped. */
    {"DecimalId", "{\"name\": \"Decimal\", \"id\": \"7.5\"}"},

    /* json.load raises -> dropped. */
    {"Broken", "{\"name\": \"Broken\", "},

    /* Valid JSON, wrong shape: data.get() raises AttributeError on a list. */
    {"ArrayRoot", "[1, 2, 3]"},

    /* Valid JSON, not a container at all. */
    {"ScalarRoot", "42"},

    /* A JSON FLOAT. int(7.5) is 7 in Python; nd_json.h is explicit that an
     * integer is not a float, so nd_json_int() refuses it and the app is
     * dropped instead. The one place this port is stricter than the Python,
     * recorded as A-3 and reachable only from a hand-written manifest. */
    {"FloatId", "{\"name\": \"Float\", \"id\": 7.5}"},

    /* No manifest.json: `continue` before anything is opened. */
    {"Unfinished", NULL},

    /* Two apps sharing an id. sort() is stable, so they stay adjacent. */
    {"TieA", "{\"name\": \"Tie A\", \"id\": \"42\"}"},
    {"TieB", "{\"name\": \"Tie B\", \"id\": \"42\"}"},
};

/* The seven of the thirteen that survive, in id order. */
static const struct {
    int32_t id;
    const char *name;
    const char *folder;
    const char *icon; /* relative to the app directory */
    const char *exec;
} SURVIVORS[] = {
    {-5, "Negative", "Negative", "icon.png", "main.py"},
    {5, "Alpha App", "Alpha", "art/big.png", "app.so"},
    {7, "Beta", "Beta", "icon.png", "main.py"},
    {12, "Padded", "Padded", "icon.png", "main.py"},
    /* Each asked for something outside its own directory and got the default
     * instead. The app survives; the escape does not. */
    {31, "Climber", "Climber", "icon.png", "main.py"},
    {32, "Absolute", "Absolute", "icon.png", "main.py"},
    {33, "Sneaky", "Sneaky", "icon.png", "main.py"},
    {42, "Tie A", "TieA", "icon.png", "main.py"},
    {42, "Tie B", "TieB", "icon.png", "main.py"},
    {999, "No Id", "NoId", "icon.png", "main.py"},
};

#define SYNTH_DIR "/NeoDCT/System/apps"

/* Paths handed to nd_mkdir_p()/nd_path_resolve() are the UNRESOLVED
 * /NeoDCT form -- the same strings the scan itself is given. Prefixing the
 * root here as well would put the tree at <root><root>/NeoDCT/... and the
 * scan would find nothing. */
static bool write_synthetic_tree(void)
{
    size_t i;

    for (i = 0u; i < ND_ARRAY_LEN(SYNTHETIC); i++) {
        char dir[ND_PATH_MAX];
        char file[ND_PATH_MAX];
        char real[ND_PATH_MAX];
        FILE *f;

        if (nd_snprintf(dir, sizeof dir, "%s/%s", SYNTH_DIR, SYNTHETIC[i].folder) != ND_OK)
            return false;
        if (nd_mkdir_p(dir, 0755u) != ND_OK)
            return false;
        if (SYNTHETIC[i].manifest == NULL)
            continue;
        if (nd_snprintf(file, sizeof file, "%s/manifest.json", dir) != ND_OK)
            return false;
        if (nd_path_resolve(real, sizeof real, file) != ND_OK)
            return false;
        f = fopen(real, "w");
        if (f == NULL)
            return false;
        (void)fputs(SYNTHETIC[i].manifest, f);
        if (fclose(f) != 0)
            return false;
    }

    /* A loose file beside the app directories. os.listdir() returns it and
     * "<dir>/loose.txt/manifest.json" simply does not exist, so it is skipped
     * by the same branch a directory without a manifest takes. */
    {
        char real[ND_PATH_MAX];
        FILE *f;

        if (nd_path_resolve(real, sizeof real, SYNTH_DIR "/loose.txt") != ND_OK)
            return false;
        f = fopen(real, "w");
        if (f == NULL)
            return false;
        (void)fputs("not an app\n", f);
        if (fclose(f) != 0)
            return false;
    }
    return true;
}

/* nd_ui_scan_apps() does the scan; the sort lives behind it in rescan_apps(),
 * which only nd_ui_init() reaches. Sorting here with the same stable insertion
 * sort the port uses is not a re-implementation of the thing under test -- the
 * thing under test is the SCAN -- it is what lets the survivors be named in id
 * order instead of in whatever order readdir happened to hand back. */
static void sort_by_id_stable(nd_app_entry *apps, size_t n)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        nd_app_entry key = apps[i];
        size_t j = i;

        while (j > 0u && apps[j - 1u].id > key.id) {
            apps[j] = apps[j - 1u];
            j--;
        }
        apps[j] = key;
    }
}

static void test_synthetic_manifests(void)
{
    char root[ND_PATH_MAX];
    nd_app_entry apps[ND_APP_MAX];
    size_t n;
    size_t i;

    if (!make_temp_dir(root, sizeof root)) {
        CHECK(false, "mkdtemp for the synthetic root");
        return;
    }
    if (nd_path_set_root(root) != ND_OK) {
        CHECK(false, "nd_path_set_root");
        return;
    }
    if (!write_synthetic_tree()) {
        CHECK(false, "writing the synthetic app tree");
        goto done;
    }

    memset(apps, 0, sizeof apps);
    n = nd_ui_scan_apps(SYNTH_DIR, apps, ND_APP_MAX);
    CHECK_INT(n, ND_ARRAY_LEN(SURVIVORS), "seven of the fourteen synthetic manifests are rejected");
    if (n != ND_ARRAY_LEN(SURVIVORS))
        goto done;

    sort_by_id_stable(apps, n);

    /* The ids alone are checked positionally. The two entries that SHARE an
     * id cannot be: sort() is stable, so their relative order is readdir's,
     * exactly as it is os.listdir()'s in the Python, and asserting one of the
     * two orders would be asserting a property of the filesystem. Every other
     * field is checked by looking the entry up by folder. */
    for (i = 0u; i < ND_ARRAY_LEN(SURVIVORS); i++)
        CHECK_INT(apps[i].id, SURVIVORS[i].id, "id order after the stable sort");

    for (i = 0u; i < ND_ARRAY_LEN(SURVIVORS); i++) {
        char want_path[ND_PATH_MAX];
        char want_icon[ND_PATH_MAX];
        const nd_app_entry *e = NULL;
        size_t j;

        (void)nd_snprintf(want_path, sizeof want_path, "%s/%s", SYNTH_DIR, SURVIVORS[i].folder);
        for (j = 0u; j < n; j++) {
            if (strcmp(apps[j].path, want_path) == 0) {
                e = &apps[j];
                break;
            }
        }
        if (e == NULL) {
            CHECK(false, SURVIVORS[i].folder);
            continue;
        }
        CHECK_INT(e->id, SURVIVORS[i].id, SURVIVORS[i].name);
        CHECK_STR(e->name, SURVIVORS[i].name, "name, or the folder name as the default");
        (void)nd_snprintf(want_icon, sizeof want_icon, "%s/%s", want_path, SURVIVORS[i].icon);
        CHECK_STR(e->icon, want_icon, "icon is joined onto the app directory");
        CHECK_STR(e->exec, SURVIVORS[i].exec, "exec as written, else main.py");
    }

    /* The two id-42 entries are adjacent and distinct.
     *
     * Found rather than indexed. This asserted apps[4] and apps[5], which
     * made it a test of how many apps happen to sort before 42 -- adding
     * three fixtures with lower ids broke it while the property it is about
     * was never in question. */
    {
        size_t tie = 0u;
        bool found = false;

        for (i = 0u; i + 1u < n; i++) {
            if (apps[i].id == 42 && apps[i + 1u].id == 42) {
                tie = i;
                found = true;
                break;
            }
        }
        CHECK(found, "the tied ids stayed adjacent");
        if (found)
            CHECK(strcmp(apps[tie].name, apps[tie + 1u].name) != 0,
                  "and are two distinct entries");
    }

    /* CODING-STANDARDS 1.5: the caller's array is a hard bound. Python's list
     * grows without one, which is the recorded deviation. */
    memset(apps, 0, sizeof apps);
    n = nd_ui_scan_apps(SYNTH_DIR, apps, 3u);
    CHECK_INT(n, 3, "the scan stops at the caller's capacity");

    /* Rejected arguments answer 0 rather than reading anything. */
    CHECK_INT(nd_ui_scan_apps(NULL, apps, ND_APP_MAX), 0, "NULL directory");
    CHECK_INT(nd_ui_scan_apps(SYNTH_DIR, NULL, ND_APP_MAX), 0, "NULL output array");
    CHECK_INT(nd_ui_scan_apps(SYNTH_DIR, apps, 0u), 0, "zero capacity");

    /* `if not os.path.exists(app_dir): os.makedirs(app_dir)` -- a scan of a
     * missing directory CREATES it and returns nothing. That is why a phone
     * with no engineering partition still gets the directory. */
    {
        char made[ND_PATH_MAX];

        CHECK(!nd_path_exists("/NeoDCT/System/engineering/apps"), "the eng dir is absent first");
        CHECK_INT(nd_ui_scan_apps("/NeoDCT/System/engineering/apps", apps, ND_APP_MAX), 0,
                  "a missing directory yields no apps");
        CHECK(nd_path_is_dir("/NeoDCT/System/engineering/apps"),
              "and the scan created it on the way past");
        /* stat() directly: nd_path_is_dir() would resolve the prefix a
         * second time. What is being checked is that the mkdir landed under
         * the staged root and not on the developer's real filesystem. */
        (void)nd_snprintf(made, sizeof made, "%s/NeoDCT/System/engineering/apps", root);
        {
            struct stat st;

            CHECK(stat(made, &st) == 0 && S_ISDIR(st.st_mode),
                  "created under the staged root, not at /");
        }
    }

done:
    (void)nd_path_set_root(NULL);
    (void)nftw(root, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
}

/* ------------------------------------------------------------------ *
 * PART 2 -- the overlay, staged the way test_ui.c stages it
 * ------------------------------------------------------------------ */

static char g_stage[ND_PATH_MAX];
static char g_overlay[ND_PATH_MAX];

static bool g_stage_is_temp;

static bool stage_overlay(void)
{
    char neodct[ND_PATH_MAX];
    char sys_link[ND_PATH_MAX];
    char sys_target[ND_PATH_MAX];
    char user[ND_PATH_MAX];
    const char *golden = getenv("NEODCT_GOLDEN");
    FILE *f;

    if (golden == NULL || golden[0] == '\0')
        return false;
    if (nd_snprintf(g_overlay, sizeof g_overlay, "%s/../../overlay", golden) != ND_OK)
        return false;
    {
        /* test_appsel.c's convention: name a directory and the staged root and
         * the rendered PNGs survive the run, which is the only way to look at
         * a notch that came out in the wrong place. */
        const char *want = getenv("NEODCT_APPREG_STAGE");

        if (want != NULL && want[0] != '\0') {
            if (nd_strlcpy(g_stage, want, sizeof g_stage) >= sizeof g_stage)
                return false;
            (void)mkdir(g_stage, 0755);
            g_stage_is_temp = false;
        } else {
            if (!make_temp_dir(g_stage, sizeof g_stage))
                return false;
            g_stage_is_temp = true;
        }
    }

    if (nd_snprintf(neodct, sizeof neodct, "%s/NeoDCT", g_stage) != ND_OK)
        return false;
    (void)mkdir(neodct, 0755);
    if (nd_snprintf(sys_link, sizeof sys_link, "%s/System", neodct) != ND_OK)
        return false;
    if (nd_snprintf(sys_target, sizeof sys_target, "%s/NeoDCT/System", g_overlay) != ND_OK)
        return false;
    if (symlink(sys_target, sys_link) != 0 && errno != EEXIST)
        return false;
    if (nd_snprintf(user, sizeof user, "%s/User", neodct) != ND_OK)
        return false;
    (void)mkdir(user, 0755);

    /* The ack file skips the first-boot modal; engineering mode ON is what
     * puts all twenty-two apps in the carousel. No wallpaper: the frames
     * here are measured, not hashed, and a black background makes the
     * scrollbar the only white in its columns. */
    {
        char path[ND_PATH_MAX];

        if (nd_snprintf(path, sizeof path, "%s/.ack_security_warning", user) != ND_OK)
            return false;
        f = fopen(path, "w");
        if (f == NULL)
            return false;
        (void)fputs("0", f);
        (void)fclose(f);

        if (nd_snprintf(path, sizeof path, "%s/settings.prop", user) != ND_OK)
            return false;
        f = fopen(path, "w");
        if (f == NULL)
            return false;
        (void)fputs("system.ui.engineering_mode=ON\n", f);
        (void)fclose(f);
    }

    return nd_path_set_root(g_stage) == ND_OK;
}

/* PIL Image.thumbnail((n, n)) -- aspect preserved, rounded, never upscaled,
 * never below 1. Written in long double so it is not the expression under
 * test recompiled: nd_image_thumbnail() computes in double. */
static void expected_thumb(int32_t w, int32_t h, int32_t box, int32_t *tw, int32_t *th)
{
    long double ratio;

    if (w <= box && h <= box) {
        *tw = w;
        *th = h;
        return;
    }
    if (w >= h) {
        ratio = (long double)h / (long double)w;
        *tw = box;
        *th = nd_max32(1, (int32_t)((long double)box * ratio + 0.5L));
    } else {
        ratio = (long double)w / (long double)h;
        *th = box;
        *tw = nd_max32(1, (int32_t)((long double)box * ratio + 0.5L));
    }
}

/* Every icon in the registry, at the size AppSelector asks for it. The
 * dimensions matter beyond "it decoded": ix = (240 - img.width) // 2 is what
 * centres the icon, so a thumbnail one pixel wide of Pillow's moves the whole
 * picture. Pillow's own answers for the three odd ones -- Koki 120x115 ->
 * 82x79, MusicPlayer 100x100 -> 82x82, LinuxShell 128x128 -> 82x82 -- are
 * reproduced by the rule above and checked against it. */
static void test_icon_geometry(nd_ui *ui)
{
    const int32_t header_y = nd_ui_header_divider_y(ui);
    const int32_t content_bottom = nd_ui_content_bottom(ui);
    const int32_t icon_y =
        header_y + nd_max32(24, nd_trunc32((double)(content_bottom - header_y) * 0.22));
    const int32_t cap =
        nd_min32(ND_APP_SELECTOR_ICON_MAX, nd_max32(24, content_bottom - icon_y - 8));
    size_t i;
    int non_square = 0;

    /* ND_APP_SELECTOR_ICON_MAX is 175 and never bites on this panel; the real
     * cap is 145 - 55 - 8. Asserted so a wrong icon_y is caught here rather
     * than as a mysterious one-pixel shift in a frame. */
    CHECK_INT(icon_y, 55, "icon_y on this panel");
    CHECK_INT(cap, 82, "the icon cap AppSelector actually asks for");

    for (i = 0u; i < nd_ui_app_count(ui); i++) {
        const nd_image *full = nd_ui_get_image(ui, nd_ui_app_list(ui, NULL)[i].icon);
        const nd_image *thumb;
        int32_t fw;
        int32_t fh;
        int32_t tw = 0;
        int32_t th = 0;

        if (full == NULL) {
            CHECK(false, "the full-size icon decoded");
            fprintf(stderr, "     %s (%s)\n", nd_ui_app_list(ui, NULL)[i].name, nd_ui_app_list(ui, NULL)[i].icon);
            continue;
        }
        /* get_image() converts to RGBA unconditionally, which is what lets
         * the paste composite through the icon's own alpha. Three of the
         * twenty-six icons are stored as palette PNGs (colour type 3). */
        CHECK(full->fmt == ND_PIXFMT_RGBA8888, "the cache always hands back RGBA");
        fw = full->w;
        fh = full->h;
        expected_thumb(fw, fh, cap, &tw, &th);
        if (fw != fh)
            non_square++;

        /* full is still in the cache; the thumbnail is a SEPARATE entry under
         * the "<path>@82" key, so neither call evicts the other here. */
        thumb = nd_ui_get_image_max(ui, nd_ui_app_list(ui, NULL)[i].icon, cap);
        if (thumb == NULL) {
            CHECK(false, "the 82 px thumbnail decoded");
            fprintf(stderr, "     %s (%s)\n", nd_ui_app_list(ui, NULL)[i].name, nd_ui_app_list(ui, NULL)[i].icon);
            continue;
        }
        CHECK_INT(thumb->w, tw, nd_ui_app_list(ui, NULL)[i].name);
        CHECK_INT(thumb->h, th, nd_ui_app_list(ui, NULL)[i].name);

        /* An icon that is fully transparent would draw nothing and still pass
         * every dimension check above, which is exactly what a wrongly
         * premultiplied resize (R-3) leaves behind at the extreme. */
        {
            bool opaque = false;
            int32_t x;
            int32_t y;

            for (y = 0; y < thumb->h && !opaque; y++) {
                for (x = 0; x < thumb->w; x++) {
                    if (nd_image_get_px(thumb, x, y).a > 0u) {
                        opaque = true;
                        break;
                    }
                }
            }
            CHECK(opaque, "the icon has ink in it");
        }
    }
    CHECK_INT(non_square, 1, "exactly one shipped icon is not square (Koki, 120x115)");
}

/* ------------------------------------------------------------------ *
 * The scrollbar, at every index
 * ------------------------------------------------------------------ */

/* Measure the notch out of the frame rather than trusting the draw call: scan
 * the notch's own columns (bar_x-4 .. bar_x-1, which the two-pixel-wide track
 * never reaches) for white and report the first and last row.
 *
 * The row window matters. Columns 228..231 also carry the page number, which
 * is drawn at (235 - w, 10) and whose ink runs to about row 26 -- scanning the
 * whole frame would report the page number as the notch. The window is the
 * track plus the notch's six rows of overhang at each end, 30..141, which
 * nothing else in this widget can reach: the 24 px title is centred and the
 * widest shipped name is nowhere near column 228, and the icon band is
 * 79..161. */
#define NOTCH_SCAN_TOP    30
#define NOTCH_SCAN_BOTTOM 141

static void measure_notch(const nd_image *frame, int32_t bar_x, int32_t *top, int32_t *bottom)
{
    int32_t x;
    int32_t y;

    *top = -1;
    *bottom = -1;
    for (y = NOTCH_SCAN_TOP; y <= NOTCH_SCAN_BOTTOM && y < frame->h; y++) {
        for (x = bar_x - 4; x < bar_x; x++) {
            if (nd_image_get_px(frame, x, y).r == 255u) {
                if (*top < 0)
                    *top = y;
                *bottom = y;
                break;
            }
        }
    }
}

static void test_scrollbar_every_index(nd_capture *cap, nd_ui *ui)
{
    nd_appsel s;
    const int32_t bar_x = nd_ui_width(ui) - 8;                  /* 232 */
    const int32_t track_top = nd_ui_header_divider_y(ui) + 6;   /* 36  */
    const int32_t track_bottom = nd_ui_content_bottom(ui) - 10; /* 135 */
    size_t i;

    CHECK_INT(bar_x, 232, "bar_x on this panel");
    CHECK_INT(track_top, 36, "track_top on this panel");
    CHECK_INT(track_bottom, 135, "track_bottom on this panel");
    if (nd_ui_app_count(ui) < 2u) {
        CHECK(false, "the scrollbar sweep needs the whole registry");
        return;
    }

    nd_appsel_init(&s, ui, "Main Menu", nd_ui_app_list(ui, NULL), nd_ui_app_count(ui), NULL);

    for (i = 0u; i < nd_ui_app_count(ui); i++) {
        const nd_image *frame;
        long double step;
        long double notch;
        int32_t want_top;
        int32_t want_bottom;
        int32_t got_top = -1;
        int32_t got_bottom = -1;

        s.selected_index = i;
        nd_appsel_draw(&s);
        frame = nd_capture_recent(cap, 0u);
        if (frame == NULL) {
            CHECK(false, "a frame per index");
            return;
        }

        /* draw.rectangle((bar_x-4, notch-3, bar_x+2, notch+3)) with float
         * corners: Pillow truncates each toward zero and the rectangle is
         * inclusive of both. */
        step = (long double)(track_bottom - track_top) / (long double)(nd_ui_app_count(ui) - 1u);
        notch = (long double)track_top + ((long double)i * step);
        want_top = (int32_t)(notch - 3.0L);
        want_bottom = (int32_t)(notch + 3.0L);

        measure_notch(frame, bar_x, &got_top, &got_bottom);
        CHECK_INT(got_top, want_top, "notch top row");
        CHECK_INT(got_bottom, want_bottom, "notch bottom row");
        CHECK_INT(got_bottom - got_top, 6, "the notch is seven rows tall at every index");
    }

    /* The track itself, checked once from an index whose notch is nowhere
     * near either end: the notch is SEVEN columns wide (bar_x-4 .. bar_x+2)
     * and overlaps the track, so at index 0 row 35 is legitimately white and
     * at the last index so are column 234 and row 136. Index 12 puts the
     * notch at rows 84..90.
     *
     * Width 2 on a vertical line grows in the MINOR axis (nd_draw.h rule 2),
     * so the track is columns 232 and 233 and no others, and nd_rect being
     * inclusive is what makes it end ON row 135. */
    s.selected_index = 12u;
    nd_appsel_draw(&s);
    {
        const nd_image *frame = nd_capture_recent(cap, 0u);
        int32_t top = -1;
        int32_t bottom = -1;

        measure_notch(frame, bar_x, &top, &bottom);
        /* The notch's step is (track_bottom - track_top) / (n_apps - 1), so
         * every app added or removed moves it. 89 with twenty-two apps, 87
         * with the twenty-three MicTest made, 84 with the twenty-four
         * Bluetooth makes, 82 with the twenty-five Sleepy makes, and 80 with
         * the twenty-six Calendar makes: 36 + 12 * 99/25 is 83.52 and the
         * notch top is three rows above it, truncated.
         * Re-cut the menu-* frames whenever this number changes -- they are a
         * regression net for the screens that did NOT move, not a reason to
         * leave the app list alone. */
        CHECK_INT(top, 80, "index 12 keeps the notch clear of both ends");
        CHECK(nd_image_get_px(frame, bar_x, track_bottom).r == 255u, "track reaches row 135");
        CHECK(nd_image_get_px(frame, bar_x + 1, track_bottom).r == 255u, "and column 233");
        CHECK(nd_image_get_px(frame, bar_x + 2, track_bottom).r == 0u, "but not column 234");
        CHECK(nd_image_get_px(frame, bar_x - 1, track_bottom).r == 0u, "nor column 231");
        CHECK(nd_image_get_px(frame, bar_x, track_bottom + 1).r == 0u, "and not row 136");
        CHECK(nd_image_get_px(frame, bar_x, track_top).r == 255u, "the track starts on row 36");
        CHECK(nd_image_get_px(frame, bar_x, track_top - 1).r == 0u, "and not row 35");
    }

    /* Index 0 puts the notch above the track's first row: trunc(36 - 3) is
     * 33, three rows of white with nothing beneath them. The last index puts
     * its bottom at 138, three rows past the track's end. Both are the
     * Python's, both are visible on a real phone, and neither is clipped. */
    s.selected_index = 0u;
    nd_appsel_draw(&s);
    {
        const nd_image *frame = nd_capture_recent(cap, 0u);
        int32_t top = -1;
        int32_t bottom = -1;

        measure_notch(frame, bar_x, &top, &bottom);
        CHECK_INT(top, 33, "the first notch starts three rows above the track");
        (void)nd_capture_save(cap, "appreg-notch-first", frame);
    }
    s.selected_index = nd_ui_app_count(ui) - 1u;
    nd_appsel_draw(&s);
    {
        const nd_image *frame = nd_capture_recent(cap, 0u);
        int32_t top = -1;
        int32_t bottom = -1;

        measure_notch(frame, bar_x, &top, &bottom);
        CHECK_INT(bottom, 138, "the last notch ends three rows below it");
        (void)nd_capture_save(cap, "appreg-notch-last", frame);
    }
}

/* Engineering mode off is fourteen apps, so the step becomes 99/13 = 7.615 and
 * every notch lands somewhere the twenty-five-app sweep never visits. No
 * golden frame covers it -- shoot_docs.py captures with engineering on -- so
 * the arithmetic is all there is to check.
 *
 * It was thirteen and 99/12 until Calendar shipped. Both counts are the
 * OVERLAY's, so a stock app added or removed lands here, which is the point:
 * the number is what the scrollbar is divided by. */
static void test_engineering_off_geometry(nd_capture *cap, nd_ui *ui)
{
    nd_appsel s;
    const int32_t bar_x = nd_ui_width(ui) - 8;
    nd_app_entry stock[ND_APP_MAX];
    size_t n;
    size_t i;

    n = nd_ui_scan_apps(ND_PATH_APPS_DIR, stock, ND_APP_MAX);
    CHECK_INT(n, 14, "fourteen stock apps with engineering off");
    if (n < 2u)
        return;
    sort_by_id_stable(stock, n);

    nd_appsel_init(&s, ui, "Main Menu", stock, n, NULL);
    for (i = 0u; i < n; i++) {
        const nd_image *frame;
        long double step = 99.0L / (long double)(n - 1u);
        long double notch = 36.0L + ((long double)i * step);
        int32_t got_top = -1;
        int32_t got_bottom = -1;

        s.selected_index = i;
        nd_appsel_draw(&s);
        frame = nd_capture_recent(cap, 0u);
        if (frame == NULL) {
            CHECK(false, "a frame per stock index");
            return;
        }
        measure_notch(frame, bar_x, &got_top, &got_bottom);
        CHECK_INT(got_top, (int32_t)(notch - 3.0L), "stock notch top");
        CHECK_INT(got_bottom, (int32_t)(notch + 3.0L), "stock notch bottom");
    }
    (void)nd_capture_save(cap, "appreg-stock-only", nd_capture_recent(cap, 0u));
}

static void run_overlay_half(void)
{
    nd_capture *cap = NULL;
    nd_fb *fb;
    nd_ui ui;

    if (nd_capture_open(&cap, "/frames", 0u) != ND_OK) {
        CHECK(false, "nd_capture_open");
        return;
    }
    fb = nd_capture_fb(cap);

    nd_vclock_enable();
    nd_ui_sim_clear(&ui);
    if (nd_ui_init(&ui, fb) != ND_OK) {
        CHECK(false, "nd_ui_init over the staged overlay");
        nd_capture_close(cap);
        nd_vclock_disable();
        return;
    }

    CHECK(nd_ui_engineering_mode(&ui), "engineering mode came from settings.prop");
    CHECK_INT(nd_ui_app_count(&ui), 26, "twenty-six apps with engineering mode on");
    CHECK(nd_ui_wallpaper(&ui) == NULL, "no wallpaper configured, so the background is black");

    test_icon_geometry(&ui);
    test_scrollbar_every_index(cap, &ui);
    test_engineering_off_geometry(cap, &ui);

    nd_ui_teardown(&ui);
    nd_ui_sim_clear(&ui);
    nd_vclock_disable();
    nd_capture_close(cap);
}

int main(void)
{
    test_synthetic_manifests();

    if (!stage_overlay()) {
        printf("test_appreg: NEODCT_GOLDEN is not set, or the overlay could not be staged\n");
        return 1;
    }
    run_overlay_half();
    (void)nd_path_set_root(NULL);
    if (g_stage_is_temp)
        (void)nftw(g_stage, rm_cb, 16, FTW_DEPTH | FTW_PHYS);
    else
        printf("test_appreg: frames in %s/frames\n", g_stage);

    printf("test_appreg: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
