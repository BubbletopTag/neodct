/* apps/Settings/main.c -- the phone's settings, app id 4.
 *
 * A one-to-one port of System/apps/Settings/main.py (310 lines): a four-item
 * VerticalList over the wallpaper picker, the memory-card screen, the
 * engineering-mode toggle, and an About page that is the only screen in the
 * eleven stock apps drawn entirely by hand -- no widget paints it, it calls
 * rectangle/line/text itself and then borrows a SoftKeyBar for the "Back".
 *
 * Two golden frames come out of here:
 *   golden/app-settings.png            run()'s VerticalList, first draw
 *   golden/app-settings-wallpaper.png  the picker after one Enter: "None",
 *                                      then the six stock wallpapers sorted
 *                                      by name.lower()
 *
 * ============ DECISION 3, AND WHY NO FRAME MOVED ============
 *
 * The Python writes into the core's live memory in three places:
 *
 *   main.py:91-93    ui.wallpaper = None / ui.load_wallpaper(path)
 *   main.py:139      ui.engineering_mode = bool(enabled)
 *   main.py:143-155  ui.apps = filtered, then ui._scan_apps_from_dir(...)
 *
 * An app is its own process now, so its `ui` is its own copy and none of the
 * three can reach the core. OPEN-QUESTIONS.md answer 3 settles what happens
 * instead: this app writes ONLY the setting, and nd_ui_refresh_after_app()
 * re-reads system.ui.wallpaper and system.ui.engineering_mode and rescans the
 * app directories after every app exit.
 *
 * THE WORK PACKAGE ASKED WHETHER THE PYTHON REPAINTS THE WALLPAPER WHILE
 * STILL INSIDE SETTINGS. It does not, and the reason is one line of
 * framework.py. SoftKeyBar.__init__ sets `is_transparent = not hasattr(ui,
 * 'softkey')` (framework.py:464) and the core assigns ui.softkey during its
 * own construction, so EVERY bar an app builds is opaque; the only reader of
 * ui.wallpaper inside the framework is `if self.is_transparent and wallpaper`
 * at framework.py:472. Nothing else on any Settings screen reads
 * ui.wallpaper, ui.apps or ui.engineering_mode. So the assignment was already
 * invisible until the app returned, both reference frames stop at a
 * VerticalList well before it, and neither needed re-cutting.
 *
 * ============ THE HELPER IS EXEC'D WITH AN UNRESOLVED PATH ============
 *
 * _show_memory_card()'s last resort is subprocess.call([SDCARD_HELPER,
 * "format", card.device]). PathRemap intercepts open(), not execve(), so the
 * Python hands the kernel the literal "/NeoDCT/System/hw/neodct-sdcard" even
 * under the test harness -- and nd_proc.h says the same thing from the other
 * side: "The path is NOT ND_ROOT-resolved: it is an executable". So it is
 * passed through verbatim. See OPEN-QUESTIONS.md ST-3 for the one behaviour
 * that differs when it is missing.
 */

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_input.h"
#include "nd_keycodes.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_proc.h"
#include "nd_settings.h"
#include "nd_storage.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_widgets.h"

#include "settings_app.h"

#include "nd_bt.h"
#include "nd_btaudio.h"
#include "nd_mic.h"

/* ------------------------------------------------------------------ *
 * The strings
 * ------------------------------------------------------------------ */

const char *const nd_setapp_exts[ND_SETAPP_EXT_COUNT] = {".jpg", ".jpeg", ".gif"};

const char *const nd_setapp_get_more_label = "Get more...";

const char *const nd_setapp_get_more_help =
    "Get more wallpapers by adding an SD card!\n"
    "\n"
    "Format a card as FAT32, make a folder called \"wallpapers\" on it, and "
    "copy your .jpg or .gif files into it.\n"
    "\n"
    "240x175 pictures look best, and a .gif that size animates. Put the card "
    "in the phone and they appear in this list. The phone can set a blank "
    "card up for you.";

const char *const nd_setapp_get_more_help_with_card =
    "Get more wallpapers from your SD card!\n"
    "\n"
    "Copy .jpg or .gif files into the \"wallpapers\" folder on the card that "
    "is in the phone and they appear in this list. 240x175 looks best, and a "
    ".gif that size animates.";

/* SDCARD_HELP is declared two thirds of the way down main.py, between
 * _show_about() and _show_memory_card(), which is why it reads as an
 * afterthought there. The two leading spaces in the folder table are load
 * bearing -- TextScroller draws them. */
const char *const nd_setapp_sdcard_help =
    "A NeoDCT memory card is a FAT32 card with these folders on it:\n"
    "\n"
    "  wallpapers   .jpg and .gif pictures\n"
    "  tones        .mp3 ringtones\n"
    "  music        your music\n"
    "  backup_db    copies of your contacts\n"
    "  update       UPDATE.ndsw system updates\n"
    "\n"
    "You can make one on a computer, or let the phone do it. Setting up only "
    "adds the folders. Formatting erases everything on the card.";

size_t nd_setapp_bt_lines(char lines[][ND_SETAPP_BT_LINE_MAX], size_t max, bool enabled,
                          bool connected)
{
    size_t n = 0u;

    if (lines == NULL || max == 0u)
        return 0u;

    if (!enabled) {
        (void)nd_strlcpy(lines[n++], "Enable", ND_SETAPP_BT_LINE_MAX);
        return n;
    }
    (void)nd_strlcpy(lines[n++], "Disable", ND_SETAPP_BT_LINE_MAX);
    if (n < max)
        (void)nd_strlcpy(lines[n++], "Scan", ND_SETAPP_BT_LINE_MAX);
    if (connected && n < max)
        (void)nd_strlcpy(lines[n++], "Disconnect", ND_SETAPP_BT_LINE_MAX);
    return n;
}

/* The USB sound card, found the way S17audio finds it, so that turning
 * Bluetooth off lands on the card this phone actually plays through. Only used
 * as a fallback -- route_to() prefers the saved bytes. */
static int bt_speaker_card(void)
{
    nd_mic_device found[8];
    size_t n = nd_mic_scan(ND_MIC_ASOUND_DIR, found, ND_ARRAY_LEN(found));
    size_t i;

    /* A capture device implies a real card; the playback side is the same
     * card on a headset. Better than guessing 0, which on this phone is the
     * onboard codec that is wired to nothing. */
    for (i = 0u; i < n; i++) {
        int card = 0;

        if (sscanf(found[i].device, "plughw:%d,", &card) == 1)
            return card;
    }
    return 1;
}

/* "Scanning..." while a scan runs. nd_infoscreen_show() blocks for a key,
 * which is right for a message and wrong for a progress state, so this draws
 * the same centred look and returns immediately. */
static void bt_say_working(nd_ui *ui, const char *what)
{
    nd_softkey bar;
    int32_t w = 0;
    int32_t h = 0;

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, nd_ui_width(ui), nd_ui_content_bottom(ui)),
                            ND_BLACK);
    nd_text_size(ui->font_n, what, &w, &h);
    (void)nd_draw_text(ui->draw, (nd_ui_width(ui) - w) / 2, (nd_ui_content_bottom(ui) - h) / 2,
                       what, ui->font_n, ND_WHITE);
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "", true);
}

/* The address of whatever is connected, or "" -- asked of bluetoothctl rather
 * than remembered, because earbuds go away without telling the phone and a
 * remembered answer would keep saying "connected" into an empty room. */
static void bt_connected_addr(char *out, size_t n)
{
    nd_btaudio_cmd cmd;
    char text[4096];
    nd_btaudio_device devices[8];
    size_t count;
    size_t i;

    out[0] = '\0';
    /* Plain "devices", not "devices Connected". The filtered form returns
     * nothing at all on the BlueZ in this image, so the loop below had no
     * candidates to confirm and this always answered "nothing is connected"
     * -- which the Pair screen reported as "Could not connect" about earbuds
     * that had in fact just connected. Each candidate is confirmed with info
     * below regardless, so the filter bought nothing even when it worked. */
    if (nd_btaudio_cmd_build(&cmd, "devices", NULL, 0) != ND_OK)
        return;
    if (nd_btaudio_run(&cmd, text, sizeof text) != 0)
        return;

    count = nd_btaudio_parse_devices(text, devices, ND_ARRAY_LEN(devices));
    for (i = 0u; i < count; i++) {
        /* "devices Connected" is a newer bluetoothctl; on an older one it
         * lists everything, so each candidate is confirmed individually. */
        nd_btaudio_cmd info;
        char detail[2048];

        if (nd_btaudio_cmd_build(&info, "info", devices[i].addr, 0) != ND_OK)
            continue;
        if (nd_btaudio_run(&info, detail, sizeof detail) != 0)
            continue;
        if (nd_btaudio_parse_connected(detail)) {
            (void)nd_strlcpy(out, devices[i].addr, n);
            return;
        }
    }
}

/* Scan, list what was found, pair and connect the one chosen. */
static void bt_scan_and_pair(nd_ui *ui)
{
    nd_btaudio_cmd cmd;
    char text[4096];
    nd_btaudio_device devices[8];
    char lines[8][ND_BTAUDIO_NAME_MAX + ND_BTAUDIO_ADDR_MAX + 4];
    const char *items[8];
    size_t count;
    size_t i;
    nd_vlist list;
    nd_softkey bar;
    int32_t choice;

    bt_say_working(ui, "Scanning...");
    if (nd_ui_present(ui) != ND_OK)
        return;

    /* The timeout IS the scan length: "scan on" runs until stopped. */
    if (nd_btaudio_cmd_build(&cmd, "scan", "on", 8) == ND_OK)
        (void)nd_btaudio_run(&cmd, NULL, 0u);

    if (nd_btaudio_cmd_build(&cmd, "devices", NULL, 0) != ND_OK)
        return;
    (void)nd_btaudio_run(&cmd, text, sizeof text);
    count = nd_btaudio_parse_devices(text, devices, ND_ARRAY_LEN(devices));

    if (count == 0u) {
        (void)nd_infoscreen_show(ui, "Nothing found", NULL, "Back");
        return;
    }

    for (i = 0u; i < count; i++) {
        (void)nd_strlcpy(lines[i], devices[i].name, sizeof lines[i]);
        items[i] = lines[i];
    }

    nd_vlist_init(&list, ui, "Devices", items, count, ND_SETAPP_ROOT_ID);
    nd_softkey_init(&bar, ui, false);
    nd_softkey_update(&bar, "Pair", false);
    choice = nd_vlist_show(&list);
    if (choice < 0 || (size_t)choice >= count)
        return;

    bt_say_working(ui, "Pairing...");
    if (nd_ui_present(ui) != ND_OK)
        return;

    /* pair, then trust, then connect. Trust is what lets the earbuds come back
     * on their own after they have been in their case; without it every
     * reconnection is a fresh pairing. */
    /* NO --timeout on pair or connect. bluetoothctl exits when the timeout
     * expires whether or not the exchange finished, and Secure Simple Pairing
     * with earbuds takes longer than it looks: the peer gives up and goes back
     * to advertising, which is heard as "ready to pair" a second time. The
     * commands return on their own when they are done. --timeout stays on
     * "scan on", which never returns by itself. */
    if (nd_btaudio_cmd_build(&cmd, "pair", devices[choice].addr, 0) == ND_OK)
        (void)nd_btaudio_run(&cmd, NULL, 0u);
    if (nd_btaudio_cmd_build(&cmd, "trust", devices[choice].addr, 0) == ND_OK)
        (void)nd_btaudio_run(&cmd, NULL, 0u);

    /* bluealsa has to be listening before the connection completes, or the
     * A2DP transport arrives with nothing to hand it to. */
    (void)nd_btaudio_bluealsa_start();

    if (nd_btaudio_cmd_build(&cmd, "connect", devices[choice].addr, 0) == ND_OK)
        (void)nd_btaudio_run(&cmd, NULL, 0u);

    {
        char addr[ND_BTAUDIO_ADDR_MAX];

        bt_connected_addr(addr, sizeof addr);
        if (addr[0] != '\0') {
            (void)nd_btaudio_route_to(addr, bt_speaker_card());
            nd_log(ND_LOG_BTAUDIO, "connected %s; audio now goes there", addr);
            (void)nd_infoscreen_show(ui, "Connected", NULL, "Back");
        } else {
            (void)nd_infoscreen_show(ui, "Could not connect", NULL, "Back");
        }
    }
}

static void show_bt_audio(nd_ui *ui)
{
    for (;;) {
        char lines[ND_SETAPP_BT_MAX_ITEMS][ND_SETAPP_BT_LINE_MAX];
        const char *items[ND_SETAPP_BT_MAX_ITEMS];
        char addr[ND_BTAUDIO_ADDR_MAX];
        nd_vlist menu;
        nd_softkey bar;
        bool enabled;
        bool connected;
        size_t count;
        size_t i;
        int32_t choice;

        if (!nd_bt_available()) {
            (void)nd_infoscreen_show(ui, "No Bluetooth", NULL, "Back");
            return;
        }

        enabled = nd_btaudio_adapter_up();
        addr[0] = '\0';
        if (enabled)
            bt_connected_addr(addr, sizeof addr);
        connected = addr[0] != '\0';

        count = nd_setapp_bt_lines(lines, ND_SETAPP_BT_MAX_ITEMS, enabled, connected);
        for (i = 0u; i < count; i++)
            items[i] = lines[i];

        nd_vlist_init(&menu, ui, "BT Audio", items, count, ND_SETAPP_ROOT_ID);
        nd_softkey_init(&bar, ui, false);
        nd_softkey_update(&bar, "Select", false);
        choice = nd_vlist_show(&menu);
        if (choice < 0)
            return;

        if (!enabled && choice == 0) {
            bt_say_working(ui, "Starting...");
            if (nd_ui_present(ui) != ND_OK)
                return;
            if (nd_btaudio_daemons_start() != ND_OK) {
                (void)nd_infoscreen_show(ui, "No Bluetooth stack", NULL, "Back");
            } else {
                nd_btaudio_cmd cmd;

                (void)nd_bt_power(0u, true);
                if (nd_btaudio_cmd_build(&cmd, "power", "on", 0) == ND_OK)
                    (void)nd_btaudio_run(&cmd, NULL, 0u);
            }
        } else if (enabled && choice == 0) {
            nd_btaudio_daemons_stop(bt_speaker_card());
            (void)nd_bt_power(0u, false);
        } else if (enabled && choice == 1) {
            bt_scan_and_pair(ui);
        } else if (enabled && choice == 2 && connected) {
            nd_btaudio_cmd cmd;

            if (nd_btaudio_cmd_build(&cmd, "disconnect", addr, 0) == ND_OK)
                (void)nd_btaudio_run(&cmd, NULL, 0u);
            (void)nd_btaudio_route_to(NULL, bt_speaker_card());
            nd_log(ND_LOG_BTAUDIO, "disconnected; audio back on the speaker");
        }

        if (nd_app_should_exit())
            return;
    }
}

const char *const nd_setapp_menu[ND_SETAPP_MENU_ITEMS] = {
    "Wallpaper", "Memory card", "Messages Style", "BT Audio", "Engineering Mode", "About"};

/* Must read the same way round as nd_msg_style_options in the Messages app:
 * index 0 is CLASSIC. test_settings_app.c pins the strings this writes. */
const char *const nd_setapp_msgstyle_options[ND_SETAPP_MSGSTYLE_ITEMS] = {"Classic", "Chat"};

const char *const nd_setapp_eng_options[ND_SETAPP_ENG_ITEMS] = {"On", "Off"};

/* ------------------------------------------------------------------ *
 * Small shared helpers
 * ------------------------------------------------------------------ */

static const char *nz(const char *s)
{
    return (s != NULL) ? s : "";
}

/* Python's str.lower() does not consult the locale and tolower() does, so on
 * a machine with a Turkish locale the two disagree about "I". Every
 * comparison in this file that the Python spells .lower() uses this. */
static char ascii_lower(char c)
{
    if (c >= 'A' && c <= 'Z')
        return (char)(c + ('a' - 'A'));
    return c;
}

static char ascii_upper(char c)
{
    if (c >= 'a' && c <= 'z')
        return (char)(c - ('a' - 'A'));
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

/* Python's `//` floors; C's `/` truncates toward zero. They part company on a
 * negative numerator, which is what (screen_w - w) becomes the moment a
 * version name is wider than the screen -- and system.os.versionname comes
 * out of a file, so it is not this app's to bound. */
static int32_t floordiv2(int32_t v)
{
    return (v >= 0) ? (v / 2) : -(((-v) + 1) / 2);
}

static bool fits(nd_ui *ui, const char *s, const nd_font *f, int32_t max_w)
{
    int32_t w = 0;
    int32_t h = 0;

    nd_ui_text_size(ui, s, f, &w, &h);
    return w <= max_w;
}

/* ------------------------------------------------------------------ *
 * _wrap_text() -- see settings_app.h for why this is its own function
 * ------------------------------------------------------------------ */

void nd_setapp_wrap_text(nd_lines *out, nd_ui *ui, const char *text, int32_t max_width,
                         const nd_font *font)
{
    char current[ND_TEXT_LINE_MAX];
    char candidate[ND_TEXT_LINE_MAX];
    char word[ND_TEXT_LINE_MAX];
    const char *p;
    bool any_word = false;

    if (out == NULL || ui == NULL)
        return;
    nd_lines_clear(out);

    current[0] = '\0';
    p = nz(text);

    for (;;) {
        size_t wlen = 0u;

        /* str.split() with no argument: any run of whitespace is one
         * separator and no empty token is ever produced. */
        while (*p != '\0' &&
               (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f'))
            p++;
        if (*p == '\0')
            break;
        while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '\v' &&
               *p != '\f') {
            if (wlen + 1u < sizeof word)
                word[wlen++] = *p;
            p++;
        }
        word[wlen] = '\0';
        any_word = true;

        /* `candidate = f"{current} {word}".strip() if current else word`.
         * The .strip() cannot bite: `current` is non-empty in that branch and
         * `word` came out of a split, so neither end has whitespace. */
        if (current[0] != '\0')
            (void)nd_snprintf(candidate, sizeof candidate, "%s %s", current, word);
        else
            (void)nd_strlcpy(candidate, word, sizeof candidate);

        if (fits(ui, candidate, font, max_width)) {
            (void)nd_strlcpy(current, candidate, sizeof current);
            continue;
        }

        /* `if current: lines.append(current)` and then `current = word`
         * UNCONDITIONALLY. A word wider than the column is therefore carried
         * as `current` and eventually emitted alone on an over-wide line; it
         * is never trimmed and never gets an ellipsis. That is the whole
         * difference from Messages' otherwise identical wrapper. */
        if (current[0] != '\0')
            (void)nd_lines_push(out, current);
        (void)nd_strlcpy(current, word, sizeof current);
    }

    /* `if not words: return [""]` -- checked here rather than up front,
     * because "no words" and "nothing left in current" are the same state. */
    if (!any_word) {
        (void)nd_lines_push(out, "");
        return;
    }
    if (current[0] != '\0')
        (void)nd_lines_push(out, current);
}

/* ------------------------------------------------------------------ *
 * _scan_wallpapers()
 * ------------------------------------------------------------------ */

bool nd_setapp_is_supported(const char *filename)
{
    size_t nl;
    size_t e;

    if (filename == NULL)
        return false;
    nl = strlen(filename);
    for (e = 0u; e < ND_SETAPP_EXT_COUNT; e++) {
        const char *ext = nd_setapp_exts[e];
        size_t sl = strlen(ext);
        size_t i;
        bool match = true;

        if (nl < sl)
            continue;
        for (i = 0u; i < sl; i++) {
            if (ascii_lower(filename[nl - sl + i]) != ext[i]) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

const char *nd_setapp_display_name(const char *filename, char *out, size_t out_sz)
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

size_t nd_setapp_wallpaper_dirs(char out[][ND_SETAPP_PATH_MAX], size_t max)
{
    char media[ND_SETAPP_DIRS_MAX][ND_STORAGE_PATH_MAX];
    size_t n_media;
    size_t n = 0u;
    size_t i;
    bool have_user = false;

    if (out == NULL || max == 0u)
        return 0u;

    n_media = nd_storage_media_dirs("wallpapers", ND_SETAPP_SYSTEM_WALLPAPER_DIR, media,
                                    (max < ND_SETAPP_DIRS_MAX) ? max : ND_SETAPP_DIRS_MAX);
    for (i = 0u; i < n_media && n < max; i++) {
        if (nd_strlcpy(out[n], media[i], ND_SETAPP_PATH_MAX) >= ND_SETAPP_PATH_MAX)
            continue;
        if (strcmp(out[n], ND_SETAPP_WALLPAPER_DIR) == 0)
            have_user = true;
        n++;
    }

    /* `if os.path.isdir(WALLPAPER_DIR) and WALLPAPER_DIR not in dirs` */
    if (n < max && !have_user && nd_path_is_dir(ND_SETAPP_WALLPAPER_DIR)) {
        if (nd_strlcpy(out[n], ND_SETAPP_WALLPAPER_DIR, ND_SETAPP_PATH_MAX) < ND_SETAPP_PATH_MAX)
            n++;
    }
    return n;
}

/* os.walk()'s pending-directory list. Heap, not stack: 64 * 256 is 16 kB and
 * CODING-STANDARDS.md section 1.5 keeps anything sized by input off the
 * stack. */
typedef struct {
    char dir[ND_SETAPP_WALK_MAX][ND_SETAPP_PATH_MAX];
    size_t n;
} walk_stack;

static bool walk_push(walk_stack *w, const char *path)
{
    if (w->n >= ND_SETAPP_WALK_MAX)
        return false;
    if (nd_strlcpy(w->dir[w->n], path, ND_SETAPP_PATH_MAX) >= ND_SETAPP_PATH_MAX)
        return false;
    w->n++;
    return true;
}

static const char *basename_of(const char *path)
{
    const char *slash = strrchr(path, '/');

    return (slash != NULL) ? slash + 1 : path;
}

/* sorted(files): byte order, which is what sorted() does to a list of str for
 * the ASCII a filename usually is. Insertion sort over one directory's
 * slice, and stable. */
static void sort_slice_by_filename(nd_wallpaper *t, size_t start, size_t end)
{
    size_t i;

    for (i = start + 1u; i < end; i++) {
        nd_wallpaper key = t[i];
        size_t j = i;

        while (j > start && strcmp(basename_of(t[j - 1u].path), basename_of(key.path)) > 0) {
            t[j] = t[j - 1u];
            j--;
        }
        t[j] = key;
    }
}

/* wallpapers.sort(key=lambda item: item["name"].lower()). Python's sort is
 * STABLE, so two wallpapers whose names differ only in case keep the order
 * the walk found them in; insertion sort is stable and n is bounded at 256. */
static void sort_by_name_lower(nd_wallpaper *t, size_t n)
{
    size_t i;

    for (i = 1u; i < n; i++) {
        nd_wallpaper key = t[i];
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
static size_t walk_one(const char *dir, nd_wallpaper *out, size_t max, size_t n, walk_stack *w)
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
        return n; /* os.walk swallows an unreadable or missing directory, and
                   * _scan_wallpapers has no os.path.exists() guard in front
                   * of it -- unlike Tones', which does */

    sub_base = w->n;
    while ((ent = readdir(d)) != NULL) {
        char child[ND_SETAPP_PATH_MAX];
        char child_real[ND_PATH_MAX];
        struct stat st;
        bool is_dir;

        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        if (nd_snprintf(child, sizeof child, "%s/%s", dir, ent->d_name) != ND_OK) {
            nd_log(ND_LOG_OS, "Path too long, skipped: %s/%s", dir, ent->d_name);
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
            /* Some filesystems -- and every one under an overlay -- answer
             * DT_UNKNOWN; os.scandir falls back to stat() in the same case. */
            if (nd_path_resolve(child_real, sizeof child_real, child) != ND_OK)
                continue;
            is_dir = (stat(child_real, &st) == 0) && S_ISDIR(st.st_mode);
        }

        if (is_dir) {
            if (!walk_push(w, child))
                nd_log(ND_LOG_OS, "Too many directories, not scanned: %s", child);
            continue;
        }
        if (!nd_setapp_is_supported(ent->d_name))
            continue;
        if (n >= max) {
            if (!warned_full) {
                nd_log(ND_LOG_OS, "More than %d wallpapers; the rest are not listed.", (int)max);
                warned_full = true;
            }
            continue;
        }

        (void)nd_setapp_display_name(ent->d_name, out[n].name, sizeof out[n].name);
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
            char tmp[ND_SETAPP_PATH_MAX];

            memcpy(tmp, w->dir[lo], sizeof tmp);
            memcpy(w->dir[lo], w->dir[hi], sizeof tmp);
            memcpy(w->dir[hi], tmp, sizeof tmp);
            lo++;
            hi--;
        }
    }
    return n;
}

size_t nd_setapp_scan(nd_wallpaper *out, size_t max)
{
    char dirs[ND_SETAPP_DIRS_MAX][ND_SETAPP_PATH_MAX];
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

    n_dirs = nd_setapp_wallpaper_dirs(dirs, ND_SETAPP_DIRS_MAX);
    for (i = 0u; i < n_dirs; i++) {
        w->n = 0u;
        if (!walk_push(w, dirs[i]))
            continue;
        while (w->n > 0u) {
            char dir[ND_SETAPP_PATH_MAX];

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
 * _show_wallpaper_menu() / _wallpaper_menu_once()
 * ------------------------------------------------------------------ */

/* _wallpaper_menu_once()'s three Python return values. */
typedef enum { MENU_DONE = 0, MENU_AGAIN } menu_again;

static menu_again wallpaper_menu_once(nd_ui *ui)
{
    nd_wallpaper *list = NULL;
    const char **names = NULL;
    nd_vlist vlist;
    nd_softkey softkey;
    nd_msgdialog dialog;
    menu_again again = MENU_DONE;
    size_t count;
    size_t i;
    int32_t selection;

    /* `try: os.makedirs(WALLPAPER_DIR, exist_ok=True) except Exception:
     * pass` -- "read-only or no user partition: stock wallpapers still
     * work", says the Python's own comment. */
    (void)nd_mkdir_p(ND_SETAPP_WALLPAPER_DIR, 0755u);

    /* owned here; freed before every return below. ND_SETAPP_MAX + 2, because
     * "None" is inserted at the front of a list that may already be full and
     * "Get more..." is appended after it: (256 + 2) * 352 = 90,816 bytes. */
    list = calloc((size_t)ND_SETAPP_MAX + 2u, sizeof *list);
    if (list == NULL) {
        nd_log_err(ND_LOG_OS, "out of memory listing wallpapers");
        return MENU_DONE;
    }

    /* wallpapers.insert(0, {"name": "None", "path": "NONE"}) -- scan into
     * list[1..] so the insert costs no shuffle. */
    count = nd_setapp_scan(list + 1, ND_SETAPP_MAX);
    (void)nd_strlcpy(list[0].name, "None", sizeof list[0].name);
    (void)nd_strlcpy(list[0].path, ND_SETAPP_WALLPAPER_NONE, sizeof list[0].path);
    count++;

    /* "Trailing entry explains where more come from, so the SD card is
     * discoverable without a manual." An empty path is the Python's None. */
    (void)nd_strlcpy(list[count].name, nd_setapp_get_more_label, sizeof list[count].name);
    list[count].path[0] = '\0';
    count++;

    /* names = [wallpaper["name"] for wallpaper in wallpapers] */
    names = calloc(count, sizeof *names);
    if (names == NULL) {
        nd_log_err(ND_LOG_OS, "out of memory listing wallpapers");
        free(list);
        return MENU_DONE;
    }
    for (i = 0u; i < count; i++)
        names[i] = list[i].name;

    nd_vlist_init(&vlist, ui, "Wallpaper", names, count, ND_SETAPP_ROOT_ID);
    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, "Select", false);

    selection = nd_vlist_show(&vlist);
    if (selection == ND_WIDGET_BACK)
        goto done;

    if (list[selection].path[0] == '\0') {
        nd_scroller help;

        nd_scroller_init(&help, ui,
                         nd_storage_is_ready() ? nd_setapp_get_more_help_with_card
                                               : nd_setapp_get_more_help,
                         NULL, NULL);
        nd_scroller_show(&help);
        again = MENU_AGAIN;
        goto done;
    }

    (void)nd_settings_set(ND_SET_UI_WALLPAPER, list[selection].path);

    /* main.py:90-93 assigned ui.wallpaper here. Decision 3: it cannot and no
     * longer needs to -- see the file header. */

    {
        char message[ND_SETAPP_NAME_MAX + 32];

        (void)nd_snprintf(message, sizeof message, "Wallpaper set to\n%s", list[selection].name);
        nd_msgdialog_init(&dialog, ui, message);
        (void)nd_msgdialog_show(&dialog);
    }

done:
    free(names);
    free(list);
    return again;
}

static void show_wallpaper_menu(nd_ui *ui)
{
    for (;;) {
        if (wallpaper_menu_once(ui) != MENU_AGAIN)
            return;
        /* Not in the Python, which had IncomingCall to unwind it. nd_app.h:
         * a loop that outlives a frame polls this. */
        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * _show_engineering_mode()
 * ------------------------------------------------------------------ */

static void show_engineering_mode(nd_ui *ui)
{
    char stored[64];
    nd_vlist menu;
    nd_softkey softkey;
    nd_msgdialog dialog;
    bool current_enabled;
    bool enabled;
    int32_t selection;

    /* _setting_is_enabled(get_setting(KEY, "ON"), default=True). The parser
     * is lib/nd_settings.c's -- nd_settings.h documents nd_setting_is_enabled
     * as this exact function, and a second copy here is how the two would
     * drift. */
    (void)nd_settings_get_copy(ND_SETAPP_ENG_KEY, ND_SET_UI_ENG_MODE_DFLT, stored, sizeof stored);
    current_enabled = nd_setting_is_enabled(stored, true);

    nd_vlist_init(&menu, ui, "Eng. Mode", nd_setapp_eng_options, ND_SETAPP_ENG_ITEMS,
                  ND_SETAPP_ROOT_ID);
    menu.selected_index = current_enabled ? 0u : 1u;

    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, "Select", false);

    selection = nd_vlist_show(&menu);
    if (selection == ND_WIDGET_BACK)
        return;

    enabled = (selection == 0);
    (void)nd_settings_set(ND_SETAPP_ENG_KEY, enabled ? "ON" : "OFF");

    /* main.py:172 called _refresh_engineering_apps(ui, enabled), which
     * flipped ui.engineering_mode and rebuilt ui.apps in the core's memory.
     * Decision 3: the core rescans after this app exits. */

    nd_msgdialog_init(&dialog, ui,
                      enabled ? "Engineering Mode set to ON." : "Engineering Mode set to OFF.");
    (void)nd_msgdialog_show(&dialog);
}

/* ------------------------------------------------------------------ *
 * Messages Style -- NOT a port
 * ------------------------------------------------------------------ */

/* Chooses between the ported Inbox/Outbox app and the conversation view.
 * Shaped exactly like show_engineering_mode() above -- the current value
 * preselected, "Select" on the softkey, a dialog confirming what was set --
 * because a settings app whose rows behave differently from each other is a
 * settings app people misread.
 *
 * The KEY and the two LABELS are the Messages app's; apps/Messages/
 * messages.h owns them and is what reads the setting. Settings and Messages
 * are separate .so files and cannot share a header, so test_settings_app.c
 * asserts the spellings match rather than trusting them to. */
static void show_messages_style(nd_ui *ui)
{
    char stored[64];
    nd_vlist menu;
    nd_softkey softkey;
    nd_msgdialog dialog;
    bool chat;
    int32_t selection;

    (void)nd_settings_get_copy(ND_SETAPP_MSGSTYLE_KEY, "CLASSIC", stored, sizeof stored);
    /* Only the literal "CHAT" preselects Chat, which is the same one-sided
     * tolerance nd_msg_style_parse() applies: anything unrecognised is
     * CLASSIC, so an upgraded phone opens on the app it had before. */
    chat = (stored[0] == 'C' || stored[0] == 'c') && (stored[1] == 'H' || stored[1] == 'h');

    nd_vlist_init(&menu, ui, "Msg. Style", nd_setapp_msgstyle_options, ND_SETAPP_MSGSTYLE_ITEMS,
                  ND_SETAPP_ROOT_ID);
    menu.selected_index = chat ? 1u : 0u;

    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, "Select", false);

    selection = nd_vlist_show(&menu);
    if (selection == ND_WIDGET_BACK)
        return;

    chat = (selection == 1);
    (void)nd_settings_set(ND_SETAPP_MSGSTYLE_KEY, chat ? "CHAT" : "CLASSIC");

    nd_msgdialog_init(&dialog, ui,
                      chat ? "Messages will open as chats." : "Messages will open as Inbox.");
    (void)nd_msgdialog_show(&dialog);
}

/* ------------------------------------------------------------------ *
 * _show_about()
 * ------------------------------------------------------------------ */

/* The one screen in this app nothing else draws. Ends with the SoftKeyBar's
 * present=False followed by an explicit ui.fb.update(ui.canvas), in that
 * order -- the bar is part of the frame, not a frame of its own. */
void nd_setapp_draw_about(nd_ui *ui)
{
    static const char *const TITLE = "NeoDCT";
    char version_name[128];
    char version_number[64];
    char build_time[128];
    char storage[3][ND_TEXT_LINE_MAX];
    nd_lines lines;
    nd_softkey softkey;
    nd_draw *d;
    int32_t screen_w;
    int32_t content_bottom;
    int32_t header_y;
    int32_t line_pad;
    int32_t y;
    int32_t w = 0;
    int32_t h = 0;
    size_t i;

    if (ui == NULL)
        return;

    d = ui->draw;
    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);
    /* max(30, int(screen_h * 0.11)) -- 30 on this panel only because the
     * floor wins; nd_ui.h says not to hard-code it. */
    header_y = nd_ui_header_divider_y(ui);

    (void)nd_settings_get_copy(ND_SET_OS_VERSIONNAME, "NeoDCT OS", version_name,
                               sizeof version_name);
    (void)nd_settings_get_copy(ND_SET_OS_VERSIONNUMBER, "", version_number, sizeof version_number);
    (void)nd_settings_get_copy(ND_SET_OS_BUILDTIME, "Unknown", build_time, sizeof build_time);

    /* `if not build_time or build_time.upper() == "NONE"`. ASCII upper: the
     * only string this can match is the literal the settings layer writes. */
    {
        bool is_none = true;
        const char *want = "NONE";

        for (i = 0u; i < 5u; i++) {
            if (ascii_upper(build_time[i]) != want[i]) {
                is_none = false;
                break;
            }
            if (want[i] == '\0')
                break;
        }
        if (build_time[0] == '\0' || is_none)
            (void)nd_strlcpy(build_time, "Unknown", sizeof build_time);
    }

    nd_ui_paint_chrome_full(ui);

    nd_ui_text_size(ui, TITLE, ui->font_n, &w, &h);
    (void)nd_draw_text(d, floordiv2(screen_w - w), 12, TITLE, ui->font_n, ND_WHITE);

    line_pad = nd_max32(10, (int32_t)((double)screen_w * 0.12)); /* 28 */
    (void)nd_draw_line(d, line_pad, header_y, screen_w - line_pad, header_y, ND_WHITE, 1);

    y = header_y + 12;

    nd_lines_init(&lines, storage, ND_ARRAY_LEN(storage));

    if (version_name[0] != '\0') {
        nd_setapp_wrap_text(&lines, ui, version_name, screen_w - 20, ui->font_s);
        /* name_lines[:2] -- at most the first two, however many there are. */
        for (i = 0u; i < lines.n && i < 2u; i++) {
            const char *line = nd_lines_at(&lines, i);

            nd_ui_text_size(ui, line, ui->font_s, &w, &h);
            if (y > content_bottom - 18)
                break;
            (void)nd_draw_text(d, floordiv2(screen_w - w), y, line, ui->font_s, ND_WHITE);
            y += 16;
        }
        y += 6;
    }

    if (version_number[0] != '\0') {
        if (y <= content_bottom - 18) {
            char label[96];

            (void)nd_snprintf(label, sizeof label, "Version: %s", version_number);
            (void)nd_draw_text(d, 10, y, label, ui->font_s, ND_GRAY);
        }
        /* The += 16 is OUTSIDE the `if y <=` in the Python too: a version
         * number that did not fit still costs its row. */
        y += 16;
    }
    if (y <= content_bottom - 18)
        (void)nd_draw_text(d, 10, y, "Build time:", ui->font_s, ND_GRAY);
    y += 16;

    nd_setapp_wrap_text(&lines, ui, build_time, screen_w - 20, ui->font_s);
    for (i = 0u; i < lines.n && i < 2u; i++) {
        if (y > content_bottom - 18)
            break;
        (void)nd_draw_text(d, 10, y, nd_lines_at(&lines, i), ui->font_s, ND_GRAY);
        y += 16;
    }

    nd_softkey_init(&softkey, ui, false);
    nd_softkey_update(&softkey, "Back", false);
    (void)nd_ui_present(ui);
}

static void show_about(nd_ui *ui)
{
    nd_setapp_draw_about(ui);

    for (;;) {
        int32_t key = nd_ui_wait_for_key(ui);

        if (key == ND_KEY_CLEAR)
            return;
        /* `while True:` with no other exit. nd_app.h requires the SIGTERM
         * flag be polled by any loop that outlives a frame. */
        if (nd_app_should_exit())
            return;
    }
}

/* ------------------------------------------------------------------ *
 * _show_memory_card()
 * ------------------------------------------------------------------ */

/* The format helper, so app_shutdown() can kill it. subprocess.call() blocks
 * until the child is done, so this is -1 except while a format is running --
 * but an incoming call during a format is exactly the case nd_app.h's
 * teardown contract is written for. */
static pid_t g_format_pid = -1;

/* subprocess.call([SDCARD_HELPER, "format", device]) -- spawn, wait, return
 * the exit status. See the file header for why the path is not resolved.
 *
 * subprocess.call INHERITS stdin/stdout/stderr; nd_proc_spec closes anything
 * it is not given, so the three are mapped through explicitly. The helper
 * logs to stderr and the serial console is where that has to land. */
static int sdcard_format(const char *device)
{
    const char *argv[4];
    nd_proc_spec spec;
    nd_proc_status st;
    pid_t pid = -1;
    int fd;

    argv[0] = ND_SETAPP_SDCARD_HELPER;
    argv[1] = "format";
    argv[2] = device;
    argv[3] = NULL;

    memset(&spec, 0, sizeof spec);
    spec.argv = argv;
    spec.owner = ND_OWNER_SYSTEM;
    for (fd = 0; fd <= 2; fd++) {
        spec.fds[spec.n_fds].child_fd = fd;
        spec.fds[spec.n_fds].our_fd = fd;
        spec.n_fds++;
    }

    if (nd_proc_spawn(ND_SETAPP_SDCARD_HELPER, &spec, &pid) != ND_OK) {
        nd_log_err(ND_LOG_OS, "cannot run %s: %s", ND_SETAPP_SDCARD_HELPER, strerror(errno));
        return -1;
    }
    g_format_pid = pid;

    memset(&st, 0, sizeof st);
    if (nd_proc_wait(pid, -1.0, &st) != ND_OK) {
        g_format_pid = -1;
        return -1;
    }
    g_format_pid = -1;
    if (st.exited)
        return st.exit_status;
    /* subprocess.call returns -signo for a signalled child. */
    return -st.signo;
}

static void show_memory_card(nd_ui *ui)
{
    nd_card card;
    nd_msgdialog dialog;
    nd_scroller help;
    char message[192];

    nd_storage_card(&card);

    if (card.state == ND_CARD_ABSENT) {
        nd_msgdialog_init(&dialog, ui, "No memory card.");
        nd_msgdialog_set_button(&dialog, "More");
        (void)nd_msgdialog_show(&dialog);
        nd_scroller_init(&help, ui, nd_setapp_sdcard_help, NULL, NULL);
        nd_scroller_show(&help);
        return;
    }

    if (card.state == ND_CARD_READY) {
        /* `card.label or "unnamed"` and `card.device or "card"` -- an empty
         * string is falsy in Python, so a blank label reads as unnamed. */
        (void)nd_snprintf(message, sizeof message, "Memory card ready.\n%s on %s",
                          (card.label[0] != '\0') ? card.label : "unnamed",
                          (card.device[0] != '\0') ? card.device : "card");
        nd_msgdialog_init(&dialog, ui, message);
        nd_msgdialog_set_button(&dialog, "More");
        (void)nd_msgdialog_show(&dialog);
        nd_scroller_init(&help, ui, nd_setapp_sdcard_help, NULL, NULL);
        nd_scroller_show(&help);
        return;
    }

    if (card.state == ND_CARD_NEEDS_SETUP) {
        /* "Mountable, just not laid out as a NeoDCT card: adding the folders
         * is enough and keeps whatever is already on it." */
        nd_msgdialog_init(&dialog, ui,
                          "This card is not set up for NeoDCT.\n"
                          "Add the NeoDCT folders to it?");
        nd_msgdialog_set_button(&dialog, "Set up");
        if (nd_msgdialog_show(&dialog) != ND_KEY_ENTER)
            return;
        if (nd_storage_setup_folders()) {
            nd_msgdialog_init(&dialog, ui, "Card is ready to use.");
            nd_msgdialog_set_button(&dialog, "OK");
        } else {
            nd_msgdialog_init(&dialog, ui,
                              "Could not write to the card.\n"
                              "It may be locked or damaged.");
            nd_msgdialog_set_button(&dialog, "OK");
            /* cancel_keys=(): the failure has to be acknowledged. */
            nd_msgdialog_set_keys(&dialog, NULL, 0u, NULL, 0u);
        }
        (void)nd_msgdialog_show(&dialog);
        return;
    }

    /* "Nothing we can mount: the only way forward is to reformat, which is
     * destructive, so make that unmistakable." */
    nd_msgdialog_init(&dialog, ui,
                      "This card cannot be read.\n"
                      "Format it? EVERYTHING ON IT WILL BE ERASED!");
    nd_msgdialog_set_button(&dialog, "Format");
    if (nd_msgdialog_show(&dialog) != ND_KEY_ENTER)
        return;
    if (card.device[0] == '\0') {
        nd_msgdialog_init(&dialog, ui, "No card device to format.");
        nd_msgdialog_set_button(&dialog, "OK");
        nd_msgdialog_set_keys(&dialog, NULL, 0u, NULL, 0u);
        (void)nd_msgdialog_show(&dialog);
        return;
    }
    if (sdcard_format(card.device) == 0) {
        nd_msgdialog_init(&dialog, ui, "Card formatted and ready.");
        nd_msgdialog_set_button(&dialog, "OK");
    } else {
        nd_msgdialog_init(&dialog, ui,
                          "Formatting failed.\nThe card may be write "
                          "protected.");
        nd_msgdialog_set_button(&dialog, "OK");
        nd_msgdialog_set_keys(&dialog, NULL, 0u, NULL, 0u);
    }
    (void)nd_msgdialog_show(&dialog);
}

/* ------------------------------------------------------------------ *
 * run()
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    if (ui == NULL)
        return 1;

    for (;;) {
        nd_vlist menu;
        nd_softkey softkey;
        int32_t selection;

        nd_vlist_init(&menu, ui, "Settings", nd_setapp_menu, ND_SETAPP_MENU_ITEMS,
                      ND_SETAPP_ROOT_ID);
        nd_softkey_init(&softkey, ui, false);
        nd_softkey_update(&softkey, "Select", false);

        selection = nd_vlist_show(&menu);
        if (selection == ND_WIDGET_BACK)
            return 0;
        if (selection == 0)
            show_wallpaper_menu(ui);
        else if (selection == 1)
            show_memory_card(ui);
        else if (selection == 2)
            show_messages_style(ui);
        else if (selection == 3)
            show_bt_audio(ui);
        else if (selection == 4)
            show_engineering_mode(ui);
        else if (selection == 5)
            show_about(ui);

        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing here holds the sound card. The one child this app can have is the
 * SD-card format helper, and it is running only while sdcard_format() is
 * blocked in nd_proc_wait() -- which is precisely when an incoming call would
 * arrive with a mkfs in flight. */
void app_shutdown(void)
{
    if (g_format_pid > 0) {
        (void)nd_proc_terminate(g_format_pid, 0.2, NULL);
        g_format_pid = -1;
    }
}
