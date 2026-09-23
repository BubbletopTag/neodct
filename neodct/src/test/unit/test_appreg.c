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

    /* ============ AND AN id THAT DOES NOT FIT IN ONE ============
     *
     * Dropped, the same way "abc" is, and for a reason that is invisible on
     * this machine: manifest_id() parsed the string form with strtol into a
     * `long` and never looked at the range. On x86-64 and on QEMU aarch64 a
     * `long` is 64 bits and 2147483648 survives to be truncated to
     * INT32_MIN; on the Luckfox it is 32 bits, so strtol SATURATES to
     * 2147483647 and sets ERANGE. Two ABIs, two menu positions, one manifest
     * -- and the list is sorted by id, so the tile moves. Both forms are
     * pinned because both paths had the bug. */
    {"HugeStringId", "{\"name\": \"Huge\", \"id\": \"2147483648\"}"},
    {"HugeNumberId", "{\"name\": \"Huge2\", \"id\": 4294967296}"},

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
    CHECK_INT(n, ND_ARRAY_LEN(SURVIVORS), "nine of the nineteen synthetic manifests are rejected");
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
        /* "NONE" EXPLICITLY. Leaving the key out used to mean the same thing,
         * because the shipped default was "NONE"; the phone boots with a
         * wallpaper now, so an absent key means THE DEFAULT WALLPAPER. This
         * test's scrollbar checks compare a track column against the same row
         * twenty columns left of it, which is only the same colour when the
         * ground is the sky -- a photograph varies with x as well as y. */
        (void)fputs("system.ui.wallpaper=NONE\n", f);
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
         * the paste composite through the icon's own alpha. Some of the
         * shipped icons are stored as palette PNGs (colour type 3), which is
         * the case this assertion is really about -- the count of them moved
         * when the engineering apps left this list and is not the point. */
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
    /* Koki's icon is 120x115 and the odd one out. It was briefly square, when
     * the Frutiger Aero set was what the phone shipped; that set is a theme
     * now (neodct/contrib/themes/FruitigerAero) and the shipped icons are the
     * phone's own again, Koki's included.
     *
     * Asserted as a count rather than dropped, because "the icons are all one
     * size except this one" is worth knowing and a second stray would
     * otherwise be silent. */
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

/* ============ THE NOTCH IS A THUMB NOW ============
 *
 * It was a seven-row white rectangle riding a white line, and it is
 * nd_theme_scrollbar's thumb -- the same object every other scrollbar in the
 * OS uses, sized to the list so that a long menu reads as long rather than
 * being a fixed seven rows.
 *
 * WHAT ROW IS ITS TOP depends on the theme, and that is the trap this comment
 * exists for. A glossy theme draws a dark border on the thumb's top row and a
 * white bevel one row inside it, so the first near-white row is top + 1. A
 * flat theme draws neither, so the first near-white row IS the top. Measuring
 * the bevel and subtracting one was right while the phone shipped the glass
 * look and is off by a pixel now that it ships the plain one.
 *
 * So the scan reports the first row carrying the thumb's ink and the caller
 * adds the bevel back only when the active theme draws one. The thumb stays
 * pinned to the pixel either way, which is what these tests are about. */
static bool thumb_bevel_row(const nd_image *frame, int32_t bar_x, int32_t y)
{
    int32_t x;

    for (x = bar_x - 2; x <= bar_x + 2; x++) {
        nd_color c = nd_image_get_px(frame, x, y);

        if (c.r > 190u && c.g > 210u && c.b > 220u)
            return true;
    }
    return false;
}

/* Whether the scrollbar drew anything in this column.
 *
 * There is no wallpaper on this screen, so the ground is the sky gradient --
 * which varies with y and NOT with x. The same row twenty columns to the left
 * is therefore exactly what the track was drawn over, and comparing against it
 * needs no separate reference render. Twenty is clear of the track's five
 * columns and of the centred icon, which is at most 80 px wide. */
static bool track_drawn(const nd_image *f, int32_t x, int32_t y)
{
    nd_color a = nd_image_get_px(f, x, y);
    nd_color b = nd_image_get_px(f, x - 20, y);
    int32_t dr = (int32_t)a.r - (int32_t)b.r;
    int32_t dg = (int32_t)a.g - (int32_t)b.g;
    int32_t db = (int32_t)a.b - (int32_t)b.b;

    if (dr < 0)
        dr = -dr;
    if (dg < 0)
        dg = -dg;
    if (db < 0)
        db = -db;
    return dr > 8 || dg > 8 || db > 8;
}

/* The thumb's top edge: one row above its bevel. -1 when there is none. */
static int32_t measure_thumb_top(const nd_image *frame, int32_t bar_x)
{
    int32_t y;

    for (y = NOTCH_SCAN_TOP; y <= NOTCH_SCAN_BOTTOM && y < frame->h; y++) {
        if (thumb_bevel_row(frame, bar_x, y))
            return ND_TH_BEVEL ? y - 1 : y;
    }
    return -1;
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

    /* nd_theme_scrollbar's arithmetic, which is the thing under test: a
     * thumb of max(10, track_h / n) rows, and a travel of what is left of the
     * track once its own height is taken out, divided by the stops. The step
     * TRUNCATES, exactly as the old notch position did. */
    {
        const int32_t track_h = track_bottom - track_top + 1;
        const size_t n = nd_ui_app_count(ui);
        int32_t thumb_h = (int32_t)((size_t)track_h / n);

        if (thumb_h < 10)
            thumb_h = 10;
        if (thumb_h > track_h)
            thumb_h = track_h;

        for (i = 0u; i < n; i++) {
            const nd_image *frame;
            double step;
            int32_t want_top;
            int32_t got_top;

            s.selected_index = i;
            nd_appsel_draw(&s);
            frame = nd_capture_recent(cap, 0u);
            if (frame == NULL) {
                CHECK(false, "a frame per index");
                return;
            }

            step = (double)(track_h - thumb_h) / (double)(n - 1u);
            want_top = track_top + nd_trunc32((double)i * step);

            got_top = measure_thumb_top(frame, bar_x);
            CHECK_INT(got_top, want_top, "thumb top row");
            /* And it never leaves the track, at either end -- which the old
             * fixed notch DID, by three rows, at both. */
            CHECK(got_top >= track_top, "the thumb starts inside the track");
            CHECK(got_top + thumb_h - 1 <= track_bottom, "and ends inside it");
        }
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
        int32_t top = measure_thumb_top(frame, bar_x);
        /* The notch's step is (track_bottom - track_top) / (n_apps - 1), so
         * every app added or removed moves it. 89 with twenty-two apps, 87
         * with the twenty-three MicTest made, 84 with the twenty-four
         * Bluetooth makes, 82 with the twenty-five Sleepy makes, 80 with the
         * twenty-six Calendar makes, and 78 with the twenty-seven Fetch made.
         *
         * 117 now, and the jump is the point rather than a surprise: the
         * thirteen engineering apps left the flat list for a submenu behind
         * one Engineering tile, so the menu is FIFTEEN entries and the step
         * grew from 99/26 to 99/14. 36 + 12 * 99/14 is 120.86 and the notch
         * top is three rows above it, truncated.
         * Re-cut the menu-* frames whenever this number changes -- they are a
         * regression net for the screens that did NOT move, not a reason to
         * leave the app list alone. */
        /* Fifteen entries over a 100-row track: the thumb is
         * max(10, 100/15) = 10 rows and the travel is (100-10)/14 = 6.43 a
         * step, so index 12 lands at 36 + trunc(77.14) = 113.
         *
         * Every app added or removed moves this, which is the point -- the
         * count is what the track is divided by. Re-cut the menu-* frames
         * whenever it changes; they are a regression net for the screens that
         * did NOT move, not a reason to leave the app list alone. */
        CHECK_INT(top, 113, "index 12 keeps the thumb clear of both ends");
        /* The track is five columns centred on bar_x, and it stops where it
         * always did. Its groove is DARKER than the ground rather than white,
         * so what is checked is that something is drawn in those columns and
         * nothing in the ones either side. */
        CHECK(track_drawn(frame, bar_x, track_bottom), "track reaches row 135");
        CHECK(track_drawn(frame, bar_x + 1, track_bottom), "and column 233");
        CHECK(!track_drawn(frame, bar_x + 4, track_bottom), "but not column 236");
        CHECK(!track_drawn(frame, bar_x - 4, track_bottom), "nor column 228");
        CHECK(!track_drawn(frame, bar_x, track_bottom + 2), "and not row 137");
        CHECK(track_drawn(frame, bar_x, track_top), "the track starts on row 36");
        CHECK(!track_drawn(frame, bar_x, track_top - 2), "and not row 34");
    }

    /* ============ AND IT NO LONGER HANGS OFF EITHER END ============
     *
     * The old notch was a fixed seven rows centred ON the position, so at
     * index 0 it started at 33 -- three rows ABOVE the track -- and at the
     * last index it ended at 138, three rows below it. Both were the Python's
     * and both were visible on a real phone: a scrollbar whose marker
     * overhangs its own rail at each extreme.
     *
     * nd_theme_scrollbar sizes the thumb and travels it inside the track, so
     * the first index puts its top ON the first row and the last puts its
     * bottom ON the last. That is a behaviour change and it is the reason to
     * assert it rather than merely re-cut the numbers. */
    s.selected_index = 0u;
    nd_appsel_draw(&s);
    {
        const nd_image *frame = nd_capture_recent(cap, 0u);

        CHECK_INT(measure_thumb_top(frame, bar_x), track_top,
                  "the first thumb starts ON the track's first row");
        (void)nd_capture_save(cap, "appreg-notch-first", frame);
    }
    s.selected_index = nd_ui_app_count(ui) - 1u;
    nd_appsel_draw(&s);
    {
        const nd_image *frame = nd_capture_recent(cap, 0u);
        const int32_t track_h = track_bottom - track_top + 1;
        int32_t thumb_h = (int32_t)((size_t)track_h / nd_ui_app_count(ui));

        if (thumb_h < 10)
            thumb_h = 10;
        CHECK_INT(measure_thumb_top(frame, bar_x) + thumb_h - 1, track_bottom,
                  "the last thumb ends ON the track's last row");
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
    {
        /* The track is rows 36..135, so 100 of them; nd_theme_scrollbar's
         * thumb is max(10, 100/n) and its travel is what is left over. */
        int32_t thumb_h = (int32_t)(100u / n);

        if (thumb_h < 10)
            thumb_h = 10;

        for (i = 0u; i < n; i++) {
            const nd_image *frame;
            double step = (double)(100 - thumb_h) / (double)(n - 1u);

            s.selected_index = i;
            nd_appsel_draw(&s);
            frame = nd_capture_recent(cap, 0u);
            if (frame == NULL) {
                CHECK(false, "a frame per stock index");
                return;
            }
            CHECK_INT(measure_thumb_top(frame, bar_x), 36 + nd_trunc32((double)i * step),
                      "stock thumb top");
        }
    }
    (void)nd_capture_save(cap, "appreg-stock-only", nd_capture_recent(cap, 0u));
}

/* ------------------------------------------------------------------ *
 * The app list is cached until something could have changed it
 * ------------------------------------------------------------------ */

/* VIRTUAL paths, exactly as the code under test uses them: nd_path_resolve()
 * prepends the staged root, so prefixing g_stage here as well would write to
 * <stage><stage>/... -- see the block at the top of this file. System is a
 * symlink to the real overlay and is never touched; User and /run are real
 * directories under the stage. */
static bool stage_write(const char *virtual_path, const char *text)
{
    char path[ND_PATH_MAX];
    FILE *f;

    if (nd_path_resolve(path, sizeof path, virtual_path) != ND_OK)
        return false;
    f = fopen(path, "w");
    if (f == NULL)
        return false;
    (void)fputs(text, f);
    return fclose(f) == 0;
}

static bool stage_mkdir_p(const char *virtual_path)
{
    return nd_mkdir_p(virtual_path, 0755u) == ND_OK;
}

/* One app directory on the "card", with the minimal manifest the scanner
 * accepts. Ids are in the .nap band so they sort after everything shipped and
 * cannot collide with a stock app. */
static bool stage_card_app(const char *dir, int32_t id)
{
    char rel[ND_PATH_MAX];
    char json[192];

    if (nd_snprintf(rel, sizeof rel, "%s/%s", ND_PATH_USER_APPS_DIR, dir) != ND_OK)
        return false;
    if (!stage_mkdir_p(rel))
        return false;
    if (nd_snprintf(rel, sizeof rel, "%s/%s/manifest.json", ND_PATH_USER_APPS_DIR, dir) != ND_OK)
        return false;
    if (nd_snprintf(json, sizeof json, "{\"name\": \"%s\", \"id\": %d}", dir, (int)id) != ND_OK)
        return false;
    return stage_write(rel, json);
}

/* ============ WHAT THIS IS ACTUALLY ABOUT ============
 *
 * The core used to re-walk all three app directories after EVERY app exit,
 * because an app that exited might have been Settings installing something.
 * One of those directories is the SD card, the walk happens before the menu
 * draws its first frame, and a card that is slow to answer cannot be
 * interrupted while it answers -- so the phone sat on a frozen home screen for
 * as long as the card took. It was reported as the menu hanging for a few
 * seconds, intermittently.
 *
 * So the walk is now conditional, and these three steps are the contract:
 * a card arriving is noticed, an app merely EXITING is not, and the
 * installer's note is. The middle one is the fix; the other two are what the
 * fix must not break. */
static void test_app_list_is_cached_until_something_changes(nd_ui *ui)
{
    size_t base = nd_ui_app_count(ui);

    /* 1. A card appears, carrying one app. The card's own state moved, so the
     *    list is rebuilt. */
    if (!stage_mkdir_p("/run/neodct") || !stage_card_app("ZZOne", 501)) {
        CHECK(false, "staged a card app");
        return;
    }
    if (!stage_write(ND_PATH_SDCARD_STATE,
                     "state=legacy\ndevice=/dev/zz0\nfstype=vfat\nlabel=ZZ\n")) {
        CHECK(false, "staged the card state file");
        return;
    }
    nd_ui_refresh_after_app(ui);
    CHECK_INT(nd_ui_app_count(ui), base + 1u, "a card appearing is noticed");

    /* 2. A second app appears with nothing announcing it, and an app exits.
     *    THE CARD IS NOT READ AGAIN -- which is the whole point. Nothing on
     *    the phone can reach this state (only nd_nap_install() adds a
     *    directory here, and it leaves a note), so the staleness is not
     *    reachable; the assertion is that the walk did not happen. */
    if (!stage_card_app("ZZTwo", 502)) {
        CHECK(false, "staged a second card app");
        return;
    }
    nd_ui_refresh_after_app(ui);
    CHECK_INT(nd_ui_app_count(ui), base + 1u, "an app exiting does not re-read the card");

    /* 3. The installer leaves its note, and the list is rebuilt. */
    if (!stage_write(ND_PATH_APPGEN, "1\n")) {
        CHECK(false, "staged the installer's note");
        return;
    }
    nd_ui_refresh_after_app(ui);
    CHECK_INT(nd_ui_app_count(ui), base + 2u, "the installer's note is noticed");

    /* 4. And the card going away is noticed too. */
    (void)stage_write(ND_PATH_SDCARD_STATE, "state=absent\n");
    nd_ui_refresh_after_app(ui);
    CHECK_INT(nd_ui_app_count(ui), base, "a card leaving is noticed");

    /* 5. ENGINEERING MODE IS PART OF THE ANSWER TOO.
     *
     * The Engineering tile is added by rescan_apps() only when the setting is
     * on, so a token that leaves the setting out makes the Settings toggle
     * stop working: it flips, and the tile neither appears nor disappears
     * until an install or a card event happens to move the token. The first
     * version of this token did exactly that. */
    if (!stage_write("/NeoDCT/User/settings.prop", "system.ui.engineering_mode=OFF\n")) {
        CHECK(false, "staged engineering mode off");
        return;
    }
    nd_ui_refresh_after_app(ui);
    CHECK_INT(nd_ui_app_count(ui), base - 1u, "turning engineering mode off drops the tile");

    if (!stage_write("/NeoDCT/User/settings.prop", "system.ui.engineering_mode=ON\n")) {
        CHECK(false, "staged engineering mode on");
        return;
    }
    nd_ui_refresh_after_app(ui);
    CHECK_INT(nd_ui_app_count(ui), base, "and turning it back on restores it");
}

/* ============ THE COUNTER'S OWN DURABILITY ============
 *
 * nd_appgen_bump() used to be fopen("wb") + fprintf + fclose, which truncates
 * the file first and puts the bytes down afterwards, with no fsync and no
 * rename. Two bytes on NAND on a phone with a removable battery, and the file
 * is read from the core's frame path -- so the window in which it is EMPTY is
 * a window in which the counter reads back as 0.
 *
 * It is temp + fsync + rename now, the shape nd_props_write_atomic() already
 * uses for everything else on /NeoDCT/User. The atomic swap itself cannot be
 * asserted from here; what can, and what the change is really for, is that a
 * bump which FAILS leaves the previous value exactly where it was rather than
 * replacing it with nothing. nd_paths.h promises the caller that `false`
 * means "the note did not land", and a truncated file is a note that landed
 * as the wrong number.
 *
 * The failure is injected by putting a DIRECTORY where the temp file goes, so
 * the create fails for every uid. A mode would say nothing to root, and this
 * suite runs as root inside QEMU. */
static void test_the_generation_counter_survives_a_failed_bump(void)
{
    char resolved[ND_PATH_MAX];
    char tmp[ND_PATH_MAX];
    unsigned long first;

    if (!stage_mkdir_p("/NeoDCT/User")) {
        CHECK(false, "staged the user partition");
        return;
    }
    if (nd_path_resolve(resolved, sizeof resolved, ND_PATH_APPGEN) != ND_OK ||
        nd_snprintf(tmp, sizeof tmp, "%s.tmp", resolved) != ND_OK) {
        CHECK(false, "resolved the counter's path");
        return;
    }
    (void)remove(resolved);
    (void)remove(tmp);

    /* From nothing at all. */
    CHECK(nd_appgen_bump(), "the first bump lands");
    first = nd_appgen_value();
    CHECK_INT((int)first, 1, "and a counter that was not there starts at one");

    /* And it reads back what it wrote, which is what makes the next bump
     * differ from this one rather than repeating it. */
    CHECK(nd_appgen_bump(), "the second bump lands");
    CHECK_INT((int)nd_appgen_value(), 2, "the counter moves by one");

    /* No litter: a temp file left beside it would be read by nothing, but it
     * is the visible half of a write that did not finish. */
    {
        struct stat st;

        CHECK(stat(tmp, &st) != 0, "no .tmp is left behind by a bump that worked");
    }

    /* Now the create cannot succeed. The OLD value has to still be there. */
    if (mkdir(tmp, 0755u) != 0) {
        CHECK(false, "staged an obstruction where the temp file goes");
        return;
    }
    CHECK(!nd_appgen_bump(), "a bump that cannot write says so");
    CHECK_INT((int)nd_appgen_value(), 2, "and leaves the counter it could not replace");
    (void)rmdir(tmp);
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
    /* Fourteen stock apps and the Engineering tile. NOT twenty-seven: the
     * thirteen engineering apps are still installed and still launchable, one
     * level down, behind that tile -- see ND_UI_ENG_TILE_ID in nd_ui.h. */
    CHECK_INT(nd_ui_app_count(&ui), 15, "fifteen apps with engineering mode on");
    /* The settings.prop this test stages names no wallpaper, so there is
      * none loaded -- the chrome painter falls back to the sky gradient
      * rather than to black, but that is nd_ui_paint_chrome's business and
      * not this one's. */
    CHECK(nd_ui_wallpaper(&ui) == NULL, "no wallpaper configured");

    test_icon_geometry(&ui);
    test_scrollbar_every_index(cap, &ui);
    test_engineering_off_geometry(cap, &ui);
    /* LAST: it stages a card and extra apps, so it must not run before the
     * tests that count the shipped app list. */
    test_app_list_is_cached_until_something_changes(&ui);
    test_the_generation_counter_survives_a_failed_bump();

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
