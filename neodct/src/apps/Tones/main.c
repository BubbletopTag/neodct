/* apps/Tones/main.c -- ringtone selection, app id 9.
 *
 * A one-to-one port of System/apps/Tones/main.py (229 lines): a two-item
 * PagedList, a "Ringing Options" menu that saves nothing, and the ringtone
 * browser -- the one screen in the eleven stock apps that drives a
 * VerticalList's scroll state by hand rather than calling show(), because it
 * has to preview the highlighted tone while the list is still up.
 *
 * golden/app-tones.png is the PagedList's first page: "Ringing Options" in
 * 24 px type with the "9-1" breadcrumb and a "Select" softkey.
 *
 * ============ WHAT THE HAND-ROLLED LOOP IS FOR ============
 *
 * VerticalList.show() blocks in wait_for_key(), and a preview that starts
 * half a second after the cursor stops moving cannot be expressed inside a
 * blocking wait. So this screen polls read_keypress(0.05) and moves
 * selected_index and window_start itself. THE THREE SCROLL RULES ARE COPIED
 * OUT OF VerticalList.show() AND MUST STAY IN STEP WITH IT -- Down clamps at
 * the end instead of wrapping, Up clamps at zero, and the number shortcuts
 * scroll the window to bring the target into view rather than returning it.
 * That last one is a real difference from show(), where 2..10 RETURNS the
 * index; here it only moves the cursor, and Enter is what chooses.
 *
 * ============ THE PREVIEW PLAYS THROUGH mpv, AND WHY ============
 *
 * The Python spawns `nice -n -10 mpv --no-video --audio-buffer=4 --quiet
 * <path>` per preview. nd_notify.h -- the module that owns the sound card --
 * exposes exactly two ways to make a noise: nd_notify_play_tone(), which
 * spawns `aplay` and therefore handles WAV only, and nd_notify_start_ring(),
 * which streams whatever `system.audio.ringtone` names and cannot be pointed
 * at an arbitrary file. EVERY SHIPPED TONE IS AN .mp3, so routing the
 * preview through play_tone() would make it silent for all sixteen of them.
 *
 * So the preview keeps mpv, spawned through nd_proc_spawn() -- fork then
 * execve, no shell, the descriptor plan built before the fork
 * (CODING-STANDARDS.md section 1.1) -- with stdout and stderr on /dev/null
 * as subprocess.DEVNULL puts them. The right long-term answer is a streaming
 * preview entry point on nd_notify.h reusing nd_tone_src; that is a change to
 * another work package's frozen header and is written up in
 * OPEN-QUESTIONS.md TN-4 rather than made here.
 *
 * ============ THREE THINGS THAT LOOK WRONG AND ARE PORTED ANYWAY ============
 *
 * 1. "Ringing Options" SAVES NOTHING. Picking Ring or Vibrate shows
 *    "Option saved (no effect yet)." and writes no setting. The dialog says
 *    so; the app is honest about it and so is this port.
 *
 * 2. THE HELP ENTRY IS A LIST ROW. "Add more..." is appended to the tone
 *    list as a row with no path, so it can be highlighted, it suppresses the
 *    preview, and the number shortcuts can land on it. It is not a softkey
 *    and not a submenu.
 *
 * 3. os.makedirs(USER_TONES_DIR) IS ATTEMPTED ON EVERY VISIT and its failure
 *    is swallowed whole (`except Exception: pass`). On a read-only user
 *    partition the screen still works, showing only the stock tones.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_notify.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_storage.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "tones.h"

/* ------------------------------------------------------------------ *
 * The strings
 * ------------------------------------------------------------------ */

const char *const nd_tones_add_more_label = "Add more...";

const char *const nd_tones_add_more_help =
    "Add more ringtones by adding an SD card!\n"
    "\n"
    "Format a card as FAT32, make a folder called \"tones\" on it, and copy "
    "your .mp3 files into it.\n"
    "\n"
    "Put the card in the phone and the tones appear in this list next to the "
    "built-in ones. The phone can set a blank card up for you from Settings.";

const char *const nd_tones_add_more_help_with_card =
    "Add more ringtones from your SD card!\n"
    "\n"
    "Copy .mp3 files into the \"tones\" folder on the card that is in the "
    "phone, and they appear in this list next to the built-in ones.";

const char *const nd_tones_menu[ND_TONES_MENU_ITEMS] = {"Ringing Options", "Ringing Tones"};

const char *const nd_tones_ringing_options[ND_TONES_RINGING_ITEMS] = {"Ring", "Vibrate"};

const char *const nd_tones_mpv_cmd[ND_TONES_MPV_ARGC] = {
    "nice", "-n", "-10", "mpv", "--no-video", "--audio-buffer=4", "--quiet"};

/* ------------------------------------------------------------------ *
 * ASCII case folding
 * ------------------------------------------------------------------ */

/* Python's str.lower() does not consult the locale and tolower() does, so on
 * a machine with a Turkish locale the two would disagree about "I". Every
 * comparison in this file that the Python spells .lower() uses this. */
static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static int ascii_lower_cmp(const char *a, const char *b)
{
    for (;;) {
        unsigned char ca = (unsigned char)ascii_lower(*a++);
        unsigned char cb = (unsigned char)ascii_lower(*b++);

        if (ca != cb)
            return (ca < cb) ? -1 : 1;
        if (ca == 0u)
            return 0;
    }
}

bool nd_tones_is_supported(const char *filename)
{
    size_t nl;
    size_t sl = sizeof ND_TONES_EXT - 1u;
    size_t i;

    if (filename == NULL)
        return false;
    nl = strlen(filename);
    if (nl < sl)
        return false;
    for (i = 0u; i < sl; i++) {
        if (ascii_lower(filename[nl - sl + i]) != ND_TONES_EXT[i])
            return false;
    }
    return true;
}

const char *nd_tones_display_name(const char *filename, char *out, size_t out_sz)
{
    const char *slash;
    const char *base;
    const char *scan;
    const char *dot = NULL;
    const char *p;
    size_t keep;

    if (out == NULL || out_sz == 0u)
        return out;
    if (filename == NULL) {
        out[0] = '\0';
        return out;
    }

    slash = strrchr(filename, '/');
    base = (slash != NULL) ? slash + 1 : filename;

    /* os.path.splitext: the dots that START a basename are part of the name,
     * so splitext(".hidden") is (".hidden", "") and not ("", ".hidden"). */
    scan = base;
    while (*scan == '.')
        scan++;
    for (p = scan; *p != '\0'; p++) {
        if (*p == '.')
            dot = p;
    }
    if (dot == NULL) {
        (void)nd_strlcpy(out, base, out_sz);
        return out;
    }
    keep = (size_t)(dot - base) + 1u; /* +1 for the terminator nd_strlcpy adds */
    (void)nd_strlcpy(out, base, (keep < out_sz) ? keep : out_sz);
    return out;
}

/* ------------------------------------------------------------------ *
 * TonePreviewPlayer
 * ------------------------------------------------------------------ */

/* One preview process, app-wide. See tones.h: app_shutdown() takes no
 * argument, and the SIGTERM teardown contract requires it to be able to kill
 * whatever this app spawned. -1 means nothing is playing. */
static pid_t g_preview_pid = -1;

pid_t nd_tones_preview_pid(void)
{
    return g_preview_pid;
}

/* execvp's lookup, which nd_proc_spawn() does not do: it takes a path.
 * A name containing a slash is a path; anything else is searched along
 * $PATH. False is subprocess.Popen's OSError. */
static bool which_exec(const char *name, char *out, size_t out_sz)
{
    const char *path;
    const char *seg;

    if (name == NULL || name[0] == '\0' || out == NULL || out_sz == 0u)
        return false;

    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) != 0)
            return false;
        return (size_t)snprintf(out, out_sz, "%s", name) < out_sz;
    }

    path = getenv("PATH");
    if (path == NULL || path[0] == '\0')
        path = "/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin";

    for (seg = path; seg != NULL;) {
        const char *colon = strchr(seg, ':');
        size_t len = (colon != NULL) ? (size_t)(colon - seg) : strlen(seg);
        int n;

        if (len == 0u)
            n = snprintf(out, out_sz, "./%s", name);
        else
            n = snprintf(out, out_sz, "%.*s/%s", (int)len, seg, name);
        if (n > 0 && (size_t)n < out_sz && access(out, X_OK) == 0)
            return true;
        seg = (colon != NULL) ? colon + 1 : NULL;
    }
    out[0] = '\0';
    return false;
}

void nd_tones_preview_play(const char *path)
{
    const char *argv[ND_TONES_MPV_ARGC + 2];
    char resolved[ND_PATH_MAX];
    char exe[ND_PATH_MAX];
    nd_proc_spec spec;
    pid_t pid = -1;
    int devnull;
    size_t i;

    if (path == NULL || path[0] == '\0') /* `if not path: return` */
        return;

    nd_tones_preview_stop();

    /* The list holds LOGICAL paths (TN-3); mpv is another program and gets a
     * real one. */
    if (nd_path_resolve(resolved, sizeof resolved, path) != ND_OK) {
        nd_log(ND_LOG_TONES, "Failed to play %s: path too long", path);
        return;
    }
    if (!which_exec(nd_tones_mpv_cmd[0], exe, sizeof exe)) {
        nd_log(ND_LOG_TONES, "Failed to play %s: %s: %s", path, nd_tones_mpv_cmd[0],
               strerror(ENOENT));
        return;
    }

    for (i = 0u; i < ND_TONES_MPV_ARGC; i++)
        argv[i] = nd_tones_mpv_cmd[i];
    argv[ND_TONES_MPV_ARGC] = resolved;
    argv[ND_TONES_MPV_ARGC + 1u] = NULL;

    /* stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL. Opened before the
     * fork, like everything else in the descriptor plan. */
    devnull = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (devnull < 0) {
        nd_log(ND_LOG_TONES, "Failed to play %s: /dev/null: %s", path, strerror(errno));
        return;
    }

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_TONE;
    spec.n_fds = 0u;
    spec.fds[spec.n_fds].child_fd = 1;
    spec.fds[spec.n_fds].our_fd = devnull;
    spec.n_fds++;
    spec.fds[spec.n_fds].child_fd = 2;
    spec.fds[spec.n_fds].our_fd = devnull;
    spec.n_fds++;

    if (nd_proc_spawn(exe, &spec, &pid) != ND_OK) {
        nd_log(ND_LOG_TONES, "Failed to play %s: %s", path, strerror(errno));
        pid = -1;
    }
    (void)close(devnull);
    g_preview_pid = pid;
}

void nd_tones_preview_stop(void)
{
    if (g_preview_pid <= 0) {
        g_preview_pid = -1;
        return;
    }
    /* terminate(), wait(timeout=0.2), then kill() -- which is exactly what
     * nd_proc_terminate does, including reaping, so the previous preview is
     * never left as a zombie in an app process that has no SIGCHLD reaper. */
    (void)nd_proc_terminate(g_preview_pid, ND_TONES_PREVIEW_GRACE, NULL);
    g_preview_pid = -1;
}

/* ------------------------------------------------------------------ *
 * _tone_dirs() and _scan_tones()
 * ------------------------------------------------------------------ */

size_t nd_tones_dirs(char out[][ND_TONES_PATH_MAX], size_t max)
{
    char media[ND_TONES_DIRS_MAX][ND_STORAGE_PATH_MAX];
    size_t n_media;
    size_t n = 0u;
    size_t i;
    bool have_user = false;

    if (out == NULL || max == 0u)
        return 0u;

    n_media = nd_storage_media_dirs("tones", ND_TONES_SYSTEM_DIR, media,
                                    (max < ND_TONES_DIRS_MAX) ? max : ND_TONES_DIRS_MAX);
    for (i = 0u; i < n_media && n < max; i++) {
        if (nd_strlcpy(out[n], media[i], ND_TONES_PATH_MAX) >= ND_TONES_PATH_MAX)
            continue;
        if (strcmp(out[n], ND_TONES_USER_DIR) == 0)
            have_user = true;
        n++;
    }

    /* `if os.path.isdir(USER_TONES_DIR) and USER_TONES_DIR not in dirs` */
    if (n < max && !have_user && nd_path_is_dir(ND_TONES_USER_DIR)) {
        if (nd_strlcpy(out[n], ND_TONES_USER_DIR, ND_TONES_PATH_MAX) < ND_TONES_PATH_MAX)
            n++;
    }
    return n;
}

/* os.walk()'s pending-directory list. Heap, not stack: 64 * 256 is 16 kB and
 * CODING-STANDARDS.md section 1.5 keeps anything sized by input off the
 * stack. */
typedef struct {
    char dir[ND_TONES_WALK_MAX][ND_TONES_PATH_MAX];
    size_t n;
} walk_stack;

static bool walk_push(walk_stack *w, const char *path)
{
    if (w->n >= ND_TONES_WALK_MAX)
        return false;
    if (nd_strlcpy(w->dir[w->n], path, ND_TONES_PATH_MAX) >= ND_TONES_PATH_MAX)
        return false;
    w->n++;
    return true;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

/* sorted(files): byte order, which is what sorted() does to a list of str
 * for the ASCII a filename usually is. Insertion sort, over a slice that is
 * one directory's worth of entries. */
static void sort_slice_by_filename(nd_tone *t, size_t start, size_t end)
{
    size_t i;

    for (i = start + 1u; i < end; i++) {
        nd_tone key = t[i];
        size_t j = i;

        while (j > start && strcmp(basename_of(t[j - 1u].path), basename_of(key.path)) > 0) {
            t[j] = t[j - 1u];
            j--;
        }
        t[j] = key;
    }
}

/* tones.sort(key=lambda item: item["name"].lower()). Python's sort is STABLE,
 * so two tones whose names differ only in case keep the order the walk found
 * them in; insertion sort is stable and n is bounded at 256. */
static void sort_by_name_lower(nd_tone *t, size_t n)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        nd_tone key = t[i];
        size_t j = i;

        while (j > 0u && ascii_lower_cmp(t[j - 1u].name, key.name) > 0) {
            t[j] = t[j - 1u];
            j--;
        }
        t[j] = key;
    }
}

/* One directory of the walk: its files go into `out`, its subdirectories go
 * onto `w`. Returns the new entry count. */
static size_t walk_one(const char *dir, nd_tone *out, size_t max, size_t n, walk_stack *w)
{
    char resolved[ND_PATH_MAX];
    size_t start = n;
    size_t sub_base;
    bool warned_full = false;
    DIR *d;
    struct dirent *ent;

    if (nd_path_resolve(resolved, sizeof resolved, dir) != ND_OK)
        return n;
    d = opendir(resolved);
    if (d == NULL)
        return n; /* os.walk swallows an unreadable directory too */

    sub_base = w->n;
    while ((ent = readdir(d)) != NULL) {
        char child[ND_TONES_PATH_MAX];
        char child_real[ND_PATH_MAX];
        struct stat st;
        bool is_dir;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (nd_snprintf(child, sizeof child, "%s/%s", dir, ent->d_name) != ND_OK) {
            nd_log(ND_LOG_TONES, "Path too long, skipped: %s/%s", dir, ent->d_name);
            continue;
        }

#ifdef DT_DIR
        if (ent->d_type == DT_DIR)
            is_dir = true;
        else if (ent->d_type != DT_UNKNOWN)
            is_dir = false;
        else
#endif
        {
            /* Some filesystems (and every one under an overlay) answer
             * DT_UNKNOWN; os.scandir falls back to stat() in exactly the
             * same case. */
            if (nd_path_resolve(child_real, sizeof child_real, child) != ND_OK)
                continue;
            is_dir = (stat(child_real, &st) == 0) && S_ISDIR(st.st_mode);
        }

        if (is_dir) {
            if (!walk_push(w, child))
                nd_log(ND_LOG_TONES, "Too many directories, not scanned: %s", child);
            continue;
        }
        if (!nd_tones_is_supported(ent->d_name))
            continue;
        if (n >= max) {
            if (!warned_full) {
                nd_log(ND_LOG_TONES, "More than %d tones; the rest are not listed.",
                       (int)max);
                warned_full = true;
            }
            continue;
        }

        (void)nd_tones_display_name(ent->d_name, out[n].name, sizeof out[n].name);
        (void)nd_strlcpy(out[n].path, child, sizeof out[n].path);
        n++;
    }
    (void)closedir(d);

    sort_slice_by_filename(out, start, n);

    /* os.walk is depth-first and descends in scandir order. A LIFO pops the
     * last push first, so the segment this directory just added is reversed
     * to put its first subdirectory back on top. */
    if (w->n > sub_base) {
        size_t lo = sub_base;
        size_t hi = w->n - 1u;

        while (lo < hi) {
            char tmp[ND_TONES_PATH_MAX];

            memcpy(tmp, w->dir[lo], sizeof tmp);
            memcpy(w->dir[lo], w->dir[hi], sizeof tmp);
            memcpy(w->dir[hi], tmp, sizeof tmp);
            lo++;
            hi--;
        }
    }
    return n;
}

size_t nd_tones_scan(nd_tone *out, size_t max)
{
    char dirs[ND_TONES_DIRS_MAX][ND_TONES_PATH_MAX];
    walk_stack *w;
    size_t n_dirs;
    size_t n = 0u;
    size_t i;

    if (out == NULL || max == 0u)
        return 0u;

    /* owned here; freed before every return below */
    w = calloc(1u, sizeof *w);
    if (w == NULL)
        return 0u;

    n_dirs = nd_tones_dirs(dirs, ND_TONES_DIRS_MAX);
    for (i = 0u; i < n_dirs; i++) {
        if (!nd_path_exists(dirs[i])) /* `if not os.path.exists(base): continue` */
            continue;
        w->n = 0u;
        if (!walk_push(w, dirs[i]))
            continue;
        while (w->n > 0u) {
            char dir[ND_TONES_PATH_MAX];

            w->n--;
            memcpy(dir, w->dir[w->n], sizeof dir);
            n = walk_one(dir, out, max, n, w);
        }
    }

    free(w);
    sort_by_name_lower(out, n);
    return n;
}

/* ------------------------------------------------------------------ *
 * _flush_input()
 * ------------------------------------------------------------------ */

/* The Python reads ui.keypad_fd directly with select(fd, 0.01); the C reads
 * the same records through ui->input, which is where an app process's key
 * channel lives and what every widget's own flush uses (nd_pagedlist.c).
 * The 0.01 s is the Python's and is what catches a record already on its
 * way; nd_input.h is explicit that it is not the same as a bare drain. */
static void flush_input(nd_ui *ui)
{
    nd_key_event ev;
    int guard;

    if (ui == NULL || ui->input == NULL)
        return;
    for (guard = 0; guard < 256; guard++) {
        if (!nd_input_read_event(ui->input, 0.01, &ev))
            break;
    }
}

/* ------------------------------------------------------------------ *
 * _show_ringing_options()
 * ------------------------------------------------------------------ */

static void show_ringing_options(nd_ui *ui)
{
    nd_vlist vlist;
    nd_softkey softkey;
    nd_msgdialog dialog;
    int32_t selection;

    nd_vlist_init(&vlist, ui, "Ringing Options", nd_tones_ringing_options,
                  ND_TONES_RINGING_ITEMS, ND_TONES_ROOT_ID);
    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, "Select", false);

    selection = nd_vlist_show(&vlist);
    if (selection == ND_WIDGET_BACK)
        return;
    /* Neither answer is written anywhere. The dialog says so. */
    nd_msgdialog_init(&dialog, ui, "Option saved (no effect yet).");
    (void)nd_msgdialog_show(&dialog);
}

/* ------------------------------------------------------------------ *
 * _show_ringing_tones()
 * ------------------------------------------------------------------ */

typedef struct {
    nd_ui *ui;
    nd_vlist *vlist;
    nd_softkey *softkey;
    const nd_tone *tones;
    size_t count;
    size_t pending_index;
    bool has_pending;
    double pending_time;
} tone_browser;

/* schedule_preview(): stop whatever is playing, and arm a new preview unless
 * the row has no path -- "Add more..." previews nothing. */
static void schedule_preview(tone_browser *b)
{
    nd_tones_preview_stop();
    if (b->tones[b->vlist->selected_index].path[0] == '\0') {
        b->has_pending = false;
        return;
    }
    b->pending_index = b->vlist->selected_index;
    b->has_pending = true;
    b->pending_time = nd_time_now();
}

static void browser_redraw(tone_browser *b)
{
    nd_softkey_update(b->softkey, "Select", false);
    nd_vlist_draw(b->vlist);
}

static void show_ringing_tones(nd_ui *ui)
{
    nd_tone *tones = NULL;
    const char **names = NULL;
    nd_vlist vlist;
    nd_softkey softkey;
    nd_msgdialog dialog;
    tone_browser b;
    size_t count;
    size_t i;

    /* `try: os.makedirs(USER_TONES_DIR, exist_ok=True) except Exception: pass` */
    (void)nd_mkdir_p(ND_TONES_USER_DIR, 0755u);

    /* owned here; freed before every return below. ND_TONES_MAX + 1 entries,
     * because "Add more..." is appended to a list that may already be full:
     * (256 + 1) * 352 = 90,464 bytes. */
    tones = calloc((size_t)ND_TONES_MAX + 1u, sizeof *tones);
    if (tones == NULL) {
        nd_log_err(ND_LOG_TONES, "out of memory listing tones");
        return;
    }

    count = nd_tones_scan(tones, ND_TONES_MAX);
    if (count == 0u) {
        nd_msgdialog_init(&dialog, ui, "No ringtones found.");
        (void)nd_msgdialog_show(&dialog);
        free(tones);
        return;
    }

    /* "A pseudo-entry at the end explains how to get more, which is the only
     * discoverable place to say 'you need an SD card for this'." */
    (void)nd_strlcpy(tones[count].name, nd_tones_add_more_label, sizeof tones[count].name);
    tones[count].path[0] = '\0';
    count++;

    /* names = [tone["name"] for tone in tones]; owned here, freed below. */
    names = calloc(count, sizeof *names);
    if (names == NULL) {
        nd_log_err(ND_LOG_TONES, "out of memory listing tones");
        free(tones);
        return;
    }
    for (i = 0u; i < count; i++)
        names[i] = tones[i].name;

    nd_vlist_init(&vlist, ui, "Tones", names, count, ND_TONES_ROOT_ID);
    nd_softkey_init(&softkey, ui, false);

    memset(&b, 0, sizeof b);
    b.ui = ui;
    b.vlist = &vlist;
    b.softkey = &softkey;
    b.tones = tones;
    b.count = count;

    flush_input(ui);
    browser_redraw(&b);

    for (;;) {
        int32_t key;

        if (b.has_pending && (nd_time_now() - b.pending_time) >= ND_TONES_PREVIEW_DELAY) {
            nd_tones_preview_play(tones[b.pending_index].path);
            b.has_pending = false;
        }

        key = nd_ui_read_keypress(ui, 0.05);
        if (key == ND_KEY_NONE) {
            if (nd_app_should_exit())
                break;
            continue;
        }

        if (key == ND_KEY_DOWN) {
            if (vlist.selected_index < count - 1u) {
                vlist.selected_index++;
                if (vlist.selected_index >= vlist.window_start + vlist.max_lines)
                    vlist.window_start++;
                schedule_preview(&b);
                browser_redraw(&b);
            }
        } else if (key == ND_KEY_UP) {
            if (vlist.selected_index > 0u) {
                vlist.selected_index--;
                if (vlist.selected_index < vlist.window_start)
                    vlist.window_start--;
                schedule_preview(&b);
                browser_redraw(&b);
            }
        } else if (key >= ND_KEY_1 && key <= ND_KEY_9) {
            /* Number shortcuts. Unlike VerticalList.show(), which RETURNS the
             * index, these only move the cursor -- Enter is what chooses. */
            size_t shortcut_idx = (size_t)(key - ND_KEY_1);

            if (shortcut_idx < count) {
                vlist.selected_index = shortcut_idx;
                if (vlist.selected_index < vlist.window_start)
                    vlist.window_start = vlist.selected_index;
                else if (vlist.selected_index >= vlist.window_start + vlist.max_lines)
                    vlist.window_start = (vlist.selected_index >= vlist.max_lines)
                                             ? (vlist.selected_index - vlist.max_lines + 1u)
                                             : 0u;
                schedule_preview(&b);
                browser_redraw(&b);
            }
        } else if (key == ND_KEY_ENTER || key == ND_KEY_KPENTER) {
            nd_tones_preview_stop();
            if (tones[vlist.selected_index].path[0] == '\0') {
                nd_scroller help;

                nd_scroller_init(&help, ui,
                                 nd_storage_is_ready() ? nd_tones_add_more_help_with_card
                                                       : nd_tones_add_more_help,
                                 NULL, NULL);
                nd_scroller_show(&help);
                flush_input(ui);
                browser_redraw(&b);
                continue;
            }
            (void)nd_settings_set(ND_RING_SETTING, tones[vlist.selected_index].path);
            {
                char message[ND_TONES_NAME_MAX + 32];

                (void)nd_snprintf(message, sizeof message, "Ringtone set to %s.",
                                  names[vlist.selected_index]);
                nd_msgdialog_init(&dialog, ui, message);
                (void)nd_msgdialog_show(&dialog);
            }
            break;
        } else if (key == ND_KEY_CLEAR) {
            nd_tones_preview_stop();
            break;
        }
    }

    /* The Python's two `return`s leave the player to garbage collection; C
     * has to say it, and app_shutdown() says it again for the SIGTERM path. */
    nd_tones_preview_stop();
    free(names);
    free(tones);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    if (ui == NULL)
        return 1;

    for (;;) {
        nd_pagedlist menu;
        int32_t selection;
        char root_id[16];

        /* PagedList's root_id is a STRING in C because callers pass compound
         * ids like "1-6"; this one is the plain integer 9. */
        (void)nd_snprintf(root_id, sizeof root_id, "%d", ND_TONES_ROOT_ID);
        nd_pagedlist_init(&menu, ui, "Tones", nd_tones_menu, ND_TONES_MENU_ITEMS, root_id, true);

        selection = nd_pagedlist_show(&menu);
        if (selection == ND_WIDGET_BACK)
            return 0;
        if (selection == 0)
            show_ringing_options(ui);
        else if (selection == 1)
            show_ringing_tones(ui);

        /* Not in the Python, which had exceptions to unwind it. nd_app.h:
         * a loop that outlives a frame polls this. */
        if (nd_app_should_exit())
            return 0;
    }
}

/* The sound card is the whole reason nd_app.h makes this mandatory: if a
 * preview is still playing when the modem thread signals an incoming call,
 * mpv holds ALSA and the phone rings silently. */
void app_shutdown(void)
{
    nd_tones_preview_stop();
}
