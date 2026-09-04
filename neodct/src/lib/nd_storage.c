/* nd_storage.c -- what is on the SD card, as the UI needs to know it.
 *
 * Nothing here mounts anything. The shell helper /bin/neodct-sdcard mounts a
 * card and publishes what it found to /run/neodct/sdcard.prop; this turns
 * that into the four questions the UI actually asks:
 *
 *   absent       no card in the slot
 *   ready        a NeoDCT card: mounted, with all five folders
 *   legacy       a NeoDCT card in the pre-0.5.0b FAT format: mounted and
 *                readable, but it cannot hold an installed app
 *   needs_setup  mountable, but not laid out as a NeoDCT card yet
 *   unformatted  a card is there but carries no filesystem we can mount
 *
 * Only "ready" hands out paths, so an app can never write into a card that is
 * about to be reformatted.
 *
 * ============ PATHS ============
 *
 * The mount point and the state file are absolute production paths, and the
 * test hook replaces them with absolute paths in the same VIRTUAL namespace:
 * everything here still goes through nd_path_resolve(), so a host test sets
 * them to something like "/t/sdcard" and lets NEODCT_ROOT do the redirection.
 * Every path handed back to a caller is likewise a virtual path -- resolving
 * on the way out would leak the test root into strings the UI displays.
 */

#include <dirent.h>
#include <stdlib.h>
#include <string.h>

#include "nd_paths.h"
#include "nd_props.h"
#include "nd_storage.h"

const char *const ND_SD_FOLDERS[ND_SD_FOLDER_COUNT] = {"wallpapers", "tones", "backup_db", "music",
                                                       "update"};

static char g_mount_point[ND_PATH_MAX] = ND_SD_MOUNT_POINT;
static char g_state_file[ND_PATH_MAX] = ND_SD_STATE_FILE;

/* Biggest listing we will walk. A card's update/ folder with more than this
 * many entries is not a NeoDCT card, and an unbounded readdir loop over
 * removable media is exactly the shape SECURITY.md warns about. */
#define ND_SD_MAX_LISTING 4096u

void nd_storage_set_paths(const char *mount_point, const char *state_file)
{
    (void)nd_strlcpy(g_mount_point, mount_point != NULL ? mount_point : ND_SD_MOUNT_POINT,
                     sizeof g_mount_point);
    (void)nd_strlcpy(g_state_file, state_file != NULL ? state_file : ND_SD_STATE_FILE,
                     sizeof g_state_file);
}

static bool has_folders(void)
{
    size_t i;

    for (i = 0u; i < ND_SD_FOLDER_COUNT; i++) {
        char path[ND_PATH_MAX];

        if (nd_snprintf(path, sizeof path, "%s/%s", g_mount_point, ND_SD_FOLDERS[i]) != ND_OK)
            return false;
        if (!nd_path_is_dir(path))
            return false;
    }
    return true;
}

void nd_storage_card(nd_card *out)
{
    /* Dialect B-2: errors="replace", so a corrupt state file still yields
     * whatever lines parsed. test_storage.py writes b"\x00\xffgarbage" and
     * expects "absent" -- which it gets because no line has an '=', NOT
     * because the file was rejected. A strict decoder would reach the same
     * answer here for the wrong reason and a different one elsewhere. */
    nd_props *values = nd_props_parse_lenient(g_state_file);
    const char *reported;
    const char *fstype;
    const char *device;
    const char *label;

    if (out == NULL)
        return;

    memset(out, 0, sizeof *out);
    (void)nd_strlcpy(out->mountpoint, g_mount_point, sizeof out->mountpoint);

    reported = nd_props_get(values, "state", "");
    fstype = nd_props_get(values, "fstype", "");
    device = nd_props_get(values, "device", "");
    label = nd_props_get(values, "label", "");
    /* Missing on a card with no arrival partition, and on every state file
     * written before there was such a thing -- the default is the empty
     * string either way, which is exactly "there is nowhere to put a
     * download". */
    (void)nd_strlcpy(out->untrusted, nd_props_get(values, "untrusted", ""),
                     sizeof out->untrusted);

    /* Computed BEFORE any of the returns below, so an absent card still
     * reports removable == true. test_storage.py pins the virtiofs case. */
    out->removable = strcmp(fstype, "virtiofs") != 0;

    /* Mounted, ours, and the wrong filesystem. Reported before the
     * absent/mounted split below because `legacy` is neither: the card IS
     * there and IS usable, just not for everything. */
    if (strcmp(reported, "legacy") == 0) {
        out->state = ND_CARD_LEGACY_FORMAT;
        (void)nd_strlcpy(out->device, device, sizeof out->device);
        (void)nd_strlcpy(out->fstype, fstype, sizeof out->fstype);
        (void)nd_strlcpy(out->label, label, sizeof out->label);
        /* No arrival directory on a FAT card -- see nd_storage_untrusted_dir,
         * whose contract is that the caller REFUSES rather than falling back
         * to the 8 MiB user partition. */
        out->untrusted[0] = '\0';
        goto done;
    }

    if (strcmp(reported, "unmountable") == 0 || strcmp(reported, "unformatted") == 0) {
        out->state = ND_CARD_UNFORMATTED;
        (void)nd_strlcpy(out->device, device, sizeof out->device);
        (void)nd_strlcpy(out->fstype, fstype, sizeof out->fstype);
        (void)nd_strlcpy(out->label, label, sizeof out->label);
        goto done;
    }

    if (strcmp(reported, "mounted") != 0 && strcmp(reported, "share") != 0 &&
        strcmp(reported, "ready") != 0) {
        /* device, fstype and label are deliberately left blank here: the
         * Python passes "" for all three on this branch even though it read
         * values for them. The arrival mount goes with them, for a reason of
         * its own -- a stale path to a card that is not there is a directory
         * a download would be written into and then lost with the card. */
        out->state = ND_CARD_ABSENT;
        out->untrusted[0] = '\0';
        goto done;
    }

    out->state = has_folders() ? ND_CARD_READY : ND_CARD_NEEDS_SETUP;
    (void)nd_strlcpy(out->device, device, sizeof out->device);
    (void)nd_strlcpy(out->fstype, fstype, sizeof out->fstype);
    (void)nd_strlcpy(out->label, label, sizeof out->label);

done:
    nd_props_free(values);
}

bool nd_storage_is_ready(void)
{
    nd_card card;

    nd_storage_card(&card);
    return card.state == ND_CARD_READY;
}

bool nd_storage_untrusted_dir(char *out, size_t out_sz)
{
    nd_card card;

    if (out == NULL || out_sz == 0u)
        return false;
    nd_storage_card(&card);
    if (card.untrusted[0] == '\0')
        return false;
    return nd_strlcpy(out, card.untrusted, out_sz) < out_sz;
}

bool nd_storage_folder(const char *name, char *out, size_t out_sz)
{
    if (name == NULL || out == NULL || out_sz == 0u)
        return false;
    if (!nd_storage_is_ready())
        return false;

    /* Note there is no check that `name` is one of the five. The Python does
     * not check either, and Koki asks for folders that are not in the list. */
    return nd_snprintf(out, out_sz, "%s/%s", g_mount_point, name) == ND_OK;
}

bool nd_storage_setup_folders(void)
{
    size_t i;

    for (i = 0u; i < ND_SD_FOLDER_COUNT; i++) {
        char path[ND_PATH_MAX];

        if (nd_snprintf(path, sizeof path, "%s/%s", g_mount_point, ND_SD_FOLDERS[i]) != ND_OK)
            return false;
        /* False on the FIRST failure; the folders created before it stay
         * created, which is what the Python's loop-then-except does. */
        if (nd_mkdir_p(path, 0755u) != ND_OK)
            return false;
    }
    return true;
}

size_t nd_storage_media_dirs(const char *kind, const char *system_dir,
                             char out[][ND_STORAGE_PATH_MAX], size_t max)
{
    char on_card[ND_PATH_MAX];
    size_t n = 0u;

    if (out == NULL || max == 0u)
        return 0u;

    /* Stock content always first: the image ships tones and wallpapers and
     * the card only adds to them. Every media app relies on the ordering. */
    if (system_dir != NULL && system_dir[0] != '\0' && nd_path_is_dir(system_dir)) {
        if (nd_strlcpy(out[n], system_dir, ND_STORAGE_PATH_MAX) < ND_STORAGE_PATH_MAX)
            n++;
    }

    if (n < max && nd_storage_folder(kind, on_card, sizeof on_card) && nd_path_is_dir(on_card)) {
        if (nd_strlcpy(out[n], on_card, ND_STORAGE_PATH_MAX) < ND_STORAGE_PATH_MAX)
            n++;
    }

    return n;
}

/* ASCII-only case-insensitive compare. Not strcasecmp: that consults the
 * locale, and Python's str.lower() does not, so on a machine with a Turkish
 * locale the two would disagree about "I". */
static int ascii_casecmp(const char *a, const char *b)
{
    for (;;) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;

        if (ca >= 'A' && ca <= 'Z')
            ca = (unsigned char)(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z')
            cb = (unsigned char)(cb + ('a' - 'A'));
        if (ca != cb)
            return ca < cb ? -1 : 1;
        if (ca == '\0')
            return 0;
    }
}

static bool ends_with_ndsw(const char *name)
{
    size_t n = strlen(name);
    size_t s = strlen(ND_SD_UPDATE_SUFFIX);

    return n >= s && ascii_casecmp(name + (n - s), ND_SD_UPDATE_SUFFIX) == 0;
}

/* The Python sorts twice: plain ascending by name, then a STABLE sort by
 * (name != "UPDATE.ndsw", name.lower()). One total comparator reproduces it,
 * because the only thing the first sort can still decide is a tie in the
 * second -- and that tie is broken by plain ascending order. */
static int update_cmp(const void *pa, const void *pb)
{
    const char *a = *(const char *const *)pa;
    const char *b = *(const char *const *)pb;
    int a_pref = strcmp(a, ND_SD_PREFERRED_UPDATE) == 0 ? 0 : 1;
    int b_pref = strcmp(b, ND_SD_PREFERRED_UPDATE) == 0 ? 0 : 1;
    int c;

    if (a_pref != b_pref)
        return a_pref - b_pref;

    c = ascii_casecmp(a, b);
    if (c != 0)
        return c;

    return strcmp(a, b);
}

size_t nd_storage_find_updates(char out[][ND_STORAGE_PATH_MAX], size_t max)
{
    char dir[ND_PATH_MAX];
    char resolved[ND_PATH_MAX];
    DIR *d = NULL;
    struct dirent *ent;
    char **names = NULL;
    size_t n = 0u;
    size_t cap = 0u;
    size_t written = 0u;
    size_t i;

    if (out == NULL || max == 0u)
        return 0u;
    if (!nd_storage_folder("update", dir, sizeof dir))
        return 0u;
    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return 0u;

    d = opendir(resolved);
    if (d == NULL)
        return 0u;

    while ((ent = readdir(d)) != NULL) {
        char full[ND_PATH_MAX];
        char *copy;

        if (n >= ND_SD_MAX_LISTING)
            break;
        if (!ends_with_ndsw(ent->d_name))
            continue;
        if (nd_snprintf(full, sizeof full, "%s/%s", dir, ent->d_name) != ND_OK)
            continue;
        /* isfile, not "not a directory": a dangling symlink named *.ndsw is
         * skipped, which is what os.path.isfile does. */
        if (!nd_path_is_file(full))
            continue;

        if (n == cap) {
            size_t grow = cap == 0u ? 16u : cap * 2u;
            /* freed below, every path out of this function */
            char **bigger = realloc(names, grow * sizeof *bigger);

            if (bigger == NULL)
                goto done;
            names = bigger;
            cap = grow;
        }
        copy = strdup(ent->d_name);
        if (copy == NULL)
            goto done;
        names[n++] = copy;
    }

    /* Guarded because qsort's first argument is declared non-null and an
     * update folder with no packages in it is the normal case. */
    if (n > 0u)
        qsort(names, n, sizeof *names, update_cmp);

    for (i = 0u; i < n && written < max; i++) {
        if (nd_snprintf(out[written], ND_STORAGE_PATH_MAX, "%s/%s", dir, names[i]) == ND_OK)
            written++;
    }

done:
    for (i = 0u; i < n; i++)
        free(names[i]);
    free(names);
    (void)closedir(d);
    return written;
}
