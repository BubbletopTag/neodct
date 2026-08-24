/* apps/PhoneBook/main.c -- the phone book, app id 1.
 *
 * A one-to-one port of System/apps/PhoneBook/main.py (214 lines). The shared
 * contact picker that file imports lives in libneodct as lib/nd_contacts.c,
 * because nd-core opens it too; see nd_contacts.h.
 *
 * ============ FOUR THINGS THAT LOOK WRONG AND ARE PORTED ANYWAY ============
 *
 * 1. TWO MENU ENTRIES DO NOTHING. run()'s if/elif chain has branches for 0,
 *    1, 2, 3 and 5. "Send entry" (4) and "1-touch dialing" (6) fall off the
 *    end. They are drawn, they are selectable, and choosing one redraws the
 *    same menu.
 *
 * 2. "Call" DOES NOT CALL. run_contact_options() item 0 paints "Calling...",
 *    the name and the number, sleeps two seconds and returns. The modem is
 *    never touched -- there is no dial, no call screen and no hang-up.
 *    README.md line 144 has advertised this as a bug since 0.3.0, and
 *    OPEN-QUESTIONS.md PB-5 writes it up. It is reproduced exactly.
 *
 * 3. SEARCH AND EDIT DISAGREE ABOUT THE EMPTY STRING. run() tests the search
 *    box with `if query:`, so confirming an empty field is treated as cancel.
 *    edit_contact_action() tests `if new_name is None:`, so confirming an
 *    empty field SAVES an empty name. Both spellings are kept: the first is
 *    `query[0] != '\0'`, the second is `!= NULL`.
 *
 * 4. ERASE HAS NO ARE-YOU-SURE. delete_contact_action() runs the DELETE the
 *    moment a row is picked. The Python comment says a dialog is planned "in
 *    M3"; until it is written, the phone deletes on one keypress.
 *
 * ============ THE MENU IS BUILT ONCE ============
 *
 * run() constructs the VerticalList before its loop, so the selection and the
 * scroll window survive a trip into a submenu -- come back from Erase and the
 * cursor is still on "Erase". Rebuilding it per iteration would reset both,
 * which is visible on screen. Same for run_contact_options().
 */

#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "nd_app.h"
#include "nd_contacts.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_log.h"
#include "nd_paths.h"
#include "nd_t9.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

#include "phonebook.h"

/* ------------------------------------------------------------------ *
 * The menus
 * ------------------------------------------------------------------ */

const char *const nd_phonebook_main_items[ND_PHONEBOOK_MAIN_ITEMS] = {
    "Search",          /* 0 */
    "Add entry",       /* 1 */
    "Edit",            /* 2 */
    "Erase",           /* 3 */
    "Send entry",      /* 4 -- no branch; does nothing */
    "Options",         /* 5 */
    "1-touch dialing"  /* 6 -- no branch; does nothing */
};

const char *const nd_phonebook_opt_items[ND_PHONEBOOK_OPT_ITEMS] = {"Type of view",
                                                                    "Memory status"};

const char *const nd_phonebook_contact_items[ND_PHONEBOOK_CONTACT_ITEMS] = {"Call", "Edit",
                                                                            "Delete",
                                                                            "Send number"};

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */

/* Python's `//` floors; C's `/` truncates toward zero. The two differ once
 * (screen_w - w) goes negative, which a long contact name makes happen. */
static int32_t floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

/* time.sleep(). Skipped while the virtual clock is running: under capture,
 * time is a frame counter (nd_vclock.h) and a real sleep moves no pixel, it
 * only makes the oracle slower. OPEN-QUESTIONS.md PB-3. */
static void dwell(double seconds)
{
    struct timespec req;

    if (seconds <= 0.0 || nd_vclock_enabled())
        return;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    while (nanosleep(&req, &req) != 0)
        break; /* EINTR only; anything else would spin */
}

/* _ensure_serial_redirect(), which main.py runs AT IMPORT TIME -- so in C it
 * runs as the first statement of app_run(), that being the earliest moment
 * this translation unit has control.
 *
 * The Python compares sys.stdout.name with the device path and skips the
 * redirect when they already match. C has no name to compare, so the same
 * question is asked of the file: if stdout is already this character device,
 * there is nothing to do. Every failure is swallowed, as the Python's bare
 * `except Exception: pass` swallows it. */
static void ensure_serial_redirect(void)
{
    const char *device = getenv(ND_ENV_SERIAL_DEVICE);
    struct stat want;
    struct stat have;
    int fd;

    if (device == NULL || device[0] == '\0')
        device = ND_PATH_SERIAL_AMA;

    if (stat(device, &want) != 0 || !S_ISCHR(want.st_mode))
        return;
    if (fstat(STDOUT_FILENO, &have) == 0 && S_ISCHR(have.st_mode) && have.st_rdev == want.st_rdev)
        return;

    fd = open(device, O_WRONLY | O_NOCTTY | O_CLOEXEC);
    if (fd < 0)
        return;
    (void)dup2(fd, STDOUT_FILENO);
    (void)dup2(fd, STDERR_FILENO);
    (void)close(fd);
}

/* ------------------------------------------------------------------ *
 * _draw_center_message
 * ------------------------------------------------------------------ */

void nd_phonebook_center_message(nd_ui *ui, const char *text, double duration, const nd_font *font,
                                 nd_color fill)
{
    int32_t screen_w;
    int32_t content_bottom;
    int32_t w = 0;
    int32_t h = 0;
    int32_t y;

    if (ui == NULL || ui->draw == NULL || text == NULL)
        return;

    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);
    if (font == NULL)
        font = ui->font_xl; /* `font = font or ui.font_xl` */

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);
    nd_ui_text_size(ui, text, font, &w, &h);
    y = nd_max32(10, floordiv(content_bottom - h, 2));
    (void)nd_draw_text(ui->draw, floordiv(screen_w - w, 2), y, text, font, fill);
    (void)nd_ui_present(ui);
    dwell(duration);
}

/* ------------------------------------------------------------------ *
 * The "Calling..." screen that does not call
 * ------------------------------------------------------------------ */

void nd_phonebook_calling_screen(nd_ui *ui, const nd_contact *contact)
{
    int32_t screen_w;
    int32_t content_bottom;
    int32_t y;

    if (ui == NULL || ui->draw == NULL || contact == NULL)
        return;

    screen_w = nd_ui_width(ui);
    content_bottom = nd_ui_content_bottom(ui);

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);

    /* max(12, int(content_bottom * 0.30)) -- int() truncates toward zero, so
     * 145 * 0.30 is 43 and not 44. The three lines are then spaced by a hard
     * 35 and 60 rather than by the fonts' heights. */
    y = nd_max32(12, nd_trunc32((double)content_bottom * 0.30));
    (void)nd_draw_text(ui->draw, 10, y, "Calling...", ui->font_xl, ND_WHITE);
    (void)nd_draw_text(ui->draw, 10, y + 35, contact->name, ui->font_n, ND_WHITE);
    (void)nd_draw_text(ui->draw, 10, y + 60, contact->number, ui->font_s, ND_WHITE);
    (void)nd_ui_present(ui);

    /* AND THAT IS ALL IT DOES. No nd_modem_dial, no call screen, no hang-up
     * key. See the file header and OPEN-QUESTIONS.md PB-5. */
    dwell(2.0);
}

nd_err nd_phonebook_options_root(char *out, size_t out_sz, size_t selection_index)
{
    if (out == NULL || out_sz == 0u)
        return ND_ERR_INVAL;
    return nd_snprintf(out, out_sz, "1-1-%zu", selection_index + 1u);
}

/* ------------------------------------------------------------------ *
 * The action helpers
 * ------------------------------------------------------------------ */

static void add_entry_action(nd_ui *ui)
{
    char name_buf[ND_TEXTINPUT_CAP];
    char num_buf[ND_TEXTINPUT_CAP];
    char err[ND_PHONEBOOK_ERR_MAX];
    nd_textinput field;
    const char *name;
    const char *number;

    if (nd_textinput_init(&field, ui, "Add Entry", "Name:", name_buf, sizeof name_buf, "",
                          ND_T9_FILTER_LETTERS) != ND_OK)
        return;
    name = nd_textinput_show(&field);
    if (name == NULL || name[0] == '\0')
        return; /* `if not name: return` -- empty is cancel here */

    if (nd_textinput_init(&field, ui, "Add Entry", "Number:", num_buf, sizeof num_buf, "",
                          ND_T9_FILTER_NUMBERS) != ND_OK)
        return;
    number = nd_textinput_show(&field);
    if (number == NULL || number[0] == '\0')
        return;

    if (nd_phonebook_insert(name_buf, num_buf, err, sizeof err) != ND_OK) {
        /* print(f"[PB] Save Error: {e}") -- stdout, no dialog, and NO
         * "Saved!": in the Python that call is inside the try block. */
        nd_log(ND_LOG_PB, "Save Error: %s", err);
        return;
    }
    nd_phonebook_center_message(ui, "Saved!", 1.0, NULL, ND_WHITE);
}

static void edit_contact_action(nd_ui *ui, const nd_contact *contact)
{
    char name_buf[ND_TEXTINPUT_CAP];
    char num_buf[ND_TEXTINPUT_CAP];
    char err[ND_PHONEBOOK_ERR_MAX];
    nd_textinput field;
    const char *new_name;
    const char *new_number;

    if (contact == NULL)
        return;

    if (nd_textinput_init(&field, ui, "Edit Name", "Name:", name_buf, sizeof name_buf,
                          contact->name, ND_T9_FILTER_LETTERS) != ND_OK)
        return;
    new_name = nd_textinput_show(&field);
    /* `if new_name is None: return` -- an empty confirm is SAVED, unlike the
     * search box two screens up. */
    if (new_name == NULL)
        return;

    if (nd_textinput_init(&field, ui, "Edit Number", "Number:", num_buf, sizeof num_buf,
                          contact->number, ND_T9_FILTER_NUMBERS) != ND_OK)
        return;
    new_number = nd_textinput_show(&field);
    if (new_number == NULL)
        return;

    if (nd_phonebook_update(contact->id, name_buf, num_buf, err, sizeof err) != ND_OK) {
        nd_log(ND_LOG_PB, "Update Error: %s", err);
        return;
    }
    nd_phonebook_center_message(ui, "Updated!", 1.0, NULL, ND_WHITE);
}

static void delete_contact_action(nd_ui *ui, const nd_contact *contact)
{
    char err[ND_PHONEBOOK_ERR_MAX];

    if (contact == NULL)
        return;

    /* No confirmation dialog. The Python's comment says "In M3 we can add a
     * 'Are you sure?' dialog here"; M3 has not happened. */
    if (nd_phonebook_delete(contact->id, err, sizeof err) != ND_OK) {
        /* The Python has no try/except here, so this failure would unwind out
         * of the app and reach the crash screen. Logging and carrying on is
         * the one deliberate divergence in this file -- OPEN-QUESTIONS.md
         * PB-6 -- because crashing the phone book over a transient sqlite
         * error is worse than the message being slightly wrong. */
        nd_log(ND_LOG_PB, "Delete Error: %s", err);
    }
    /* "Erased" -- no exclamation mark, unlike "Saved!" and "Updated!". */
    nd_phonebook_center_message(ui, "Erased", 1.0, NULL, ND_WHITE);
}

/* ------------------------------------------------------------------ *
 * The submenus
 * ------------------------------------------------------------------ */

/* The menu that appears after finding a contact in Search. `contact` is a
 * COPY the caller owns: run_contact_options() keeps using the tuple it was
 * handed even after Edit has rewritten the row, and the title borrows
 * contact->name for as long as the list lives. */
static void run_contact_options(nd_ui *ui, const nd_contact *contact, const char *header_root)
{
    nd_vlist options_list;
    nd_softkey softkey;

    nd_vlist_init(&options_list, ui, contact->name, nd_phonebook_contact_items,
                  ND_PHONEBOOK_CONTACT_ITEMS, ND_PHONEBOOK_APP_ID);
    /* app_id is a compound string ("1-1-3"), which nd_vlist_init cannot take. */
    nd_header_init(&options_list.header, ui, header_root);
    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        int32_t sel;

        nd_softkey_update(&softkey, "Select", true);
        sel = nd_vlist_show(&options_list);

        if (sel == ND_WIDGET_BACK)
            return;

        if (sel == 0) {
            nd_phonebook_calling_screen(ui, contact);
        } else if (sel == 1) {
            edit_contact_action(ui, contact);
            return; /* back to the search results, not to this menu */
        } else if (sel == 2) {
            delete_contact_action(ui, contact);
            return; /* must return -- the row is gone */
        } else if (sel == 3) {
            nd_phonebook_center_message(ui, "Sent!", 1.0, NULL, ND_WHITE);
        }

        if (nd_app_should_exit())
            return;
    }
}

static void run_options_submenu(nd_ui *ui)
{
    nd_vlist opt_list;
    nd_softkey softkey;

    nd_vlist_init(&opt_list, ui, "Options", nd_phonebook_opt_items, ND_PHONEBOOK_OPT_ITEMS,
                  ND_PHONEBOOK_APP_ID);
    nd_header_init(&opt_list.header, ui, "1-6");
    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        int32_t selection;

        nd_softkey_update(&softkey, "Select", true);
        selection = nd_vlist_show(&opt_list);

        if (selection == ND_WIDGET_BACK)
            return;
        /* Both entries print a bare, untagged line and draw nothing. */
        if (selection == 0)
            nd_log_line("Changing View Type...");
        else if (selection == 1)
            nd_log_line("Checking Memory...");

        if (nd_app_should_exit())
            return;
    }
}

/* Menu item 0. Ask for a name, filter the list by it, then open the per-
 * contact menu on whatever was picked. */
static void search_action(nd_ui *ui)
{
    char query_buf[ND_TEXTINPUT_CAP];
    char root[16];
    nd_textinput search_input;
    nd_contact target;
    size_t selection_index = 0u;
    const char *query;

    if (nd_textinput_init(&search_input, ui, "Search", "Name:", query_buf, sizeof query_buf, "",
                          ND_T9_FILTER_LETTERS) != ND_OK)
        return;
    query = nd_textinput_show(&search_input);

    /* `if query:` -- None AND "" are both cancel. */
    if (query == NULL || query[0] == '\0')
        return;

    if (!nd_contacts_pick(ui, "Results", "Options", query_buf, "1-1", &target, &selection_index))
        return;

    if (nd_phonebook_options_root(root, sizeof root, selection_index) != ND_OK)
        return;
    run_contact_options(ui, &target, root);
}

/* ------------------------------------------------------------------ *
 * run(ui)
 * ------------------------------------------------------------------ */

int app_run(nd_ui *ui)
{
    nd_vlist main_list;
    nd_softkey softkey;

    if (ui == NULL)
        return 1;

    ensure_serial_redirect();

    /* Built ONCE, outside the loop -- see the file header. */
    nd_vlist_init(&main_list, ui, "Phonebook", nd_phonebook_main_items, ND_PHONEBOOK_MAIN_ITEMS,
                  ND_PHONEBOOK_APP_ID);
    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        nd_contact target;
        int32_t selection;

        nd_softkey_update(&softkey, "Select", true);
        selection = nd_vlist_show(&main_list);

        if (selection == ND_WIDGET_BACK)
            return 0;

        if (selection == 0) {
            search_action(ui);
        } else if (selection == 1) {
            add_entry_action(ui);
        } else if (selection == 2) {
            /* Select from the FULL list, then edit. */
            if (nd_contacts_pick(ui, "Edit", "Edit", NULL, "1-3", &target, NULL))
                edit_contact_action(ui, &target);
        } else if (selection == 3) {
            /* Select from the FULL list, then delete. */
            if (nd_contacts_pick(ui, "Erase", "Erase", NULL, "1-4", &target, NULL))
                delete_contact_action(ui, &target);
        } else if (selection == 5) {
            run_options_submenu(ui);
        }
        /* 4 "Send entry" and 6 "1-touch dialing" have no branch at all. */

        if (nd_app_should_exit())
            return 0;
    }
}

/* Nothing is held open between screens: every database handle is closed by
 * the statement that opened it, and no child process is ever spawned. The
 * symbol exists because nd_app.h requires every app to export one. */
void app_shutdown(void) {}
