/* nd_contacts.c -- System/apps/PhoneBook/shared/list_ui.py, the contact
 * picker three different callers open.
 *
 * It is in libneodct rather than in apps/PhoneBook because nd-core opens it
 * too, from handle_input() on Up/Down at the home screen, and the core never
 * loads an app.so. See the nd_contacts.h header comment for the full
 * argument.
 *
 * ============ THE PYTHON'S while True: RUNS EXACTLY ONCE ============
 *
 * list_ui.py wraps the softkey paint and vlist.show() in `while True:`, and
 * both arms of the body return. The loop is kept here because the Python has
 * it and because deleting it would quietly change what a future edit inside
 * the body means, not because it can iterate.
 *
 * ============ THE EMPTY STATE HOLDS THE SCREEN FOR 1.5 s ============
 *
 * With no rows, list_ui paints one centred word and sleeps before returning,
 * so the caller's next screen does not wipe it in the same frame. That sleep
 * is real on the phone. It is skipped when the virtual clock is running --
 * under capture, time is a frame counter (nd_vclock.h) and a real sleep moves
 * no pixel, it only makes the oracle slower. Recorded in OPEN-QUESTIONS.md as
 * PB-3.
 */

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "nd_contacts.h"
#include "nd_db.h"
#include "nd_draw.h"
#include "nd_font.h"
#include "nd_log.h"
#include "nd_text.h"
#include "nd_types.h"
#include "nd_ui.h"
#include "nd_vclock.h"
#include "nd_widgets.h"

/* Python's `//` floors; C's `/` truncates toward zero. They part company on a
 * negative numerator, which is exactly what (screen_w - w) is for a string
 * wider than the screen -- and a contact name can be. */
static int32_t floordiv(int32_t a, int32_t b)
{
    int32_t q = a / b;

    if ((a % b != 0) && ((a < 0) != (b < 0)))
        q--;
    return q;
}

/* time.sleep(). See the header comment for why capture mode skips it. */
static void dwell(double seconds)
{
    struct timespec req;

    if (seconds <= 0.0 || nd_vclock_enabled())
        return;
    req.tv_sec = (time_t)seconds;
    req.tv_nsec = (long)((seconds - (double)req.tv_sec) * 1e9);
    while (nanosleep(&req, &req) != 0) {
        /* EINTR only; any other failure means the request was nonsense and
         * retrying it would spin. */
        break;
    }
}

/* The "No Results" / "No Contacts" screen, drawn exactly as list_ui draws it:
 * clear rows 0..content_bottom, centre the word with font_n, present, hold. */
static void draw_empty(nd_ui *ui, const char *search_query)
{
    const char *msg = (search_query != NULL && search_query[0] != '\0') ? "No Results"
                                                                       : "No Contacts";
    int32_t screen_w = nd_ui_width(ui);
    int32_t content_bottom = nd_ui_content_bottom(ui);
    int32_t w = 0;
    int32_t h = 0;
    int32_t y;

    (void)nd_draw_rect_fill(ui->draw, ND_RECT(0, 0, screen_w, content_bottom), ND_BLACK);
    nd_ui_text_size(ui, msg, ui->font_n, &w, &h);
    y = nd_max32(10, floordiv(content_bottom - h, 2));
    (void)nd_draw_text(ui->draw, floordiv(screen_w - w, 2), y, msg, ui->font_n, ND_WHITE);
    (void)nd_ui_present(ui);
    dwell(1.5);
}

bool nd_contacts_pick(nd_ui *ui, const char *title, const char *btn_text,
                      const char *search_query, const char *header_root, nd_contact *out,
                      size_t *out_index)
{
    nd_contact *contacts = NULL; /* owned here; freed before every return */
    const char **names = NULL;   /* ditto -- borrowed by the list, so both
                                  * outlive nd_vlist_show()                */
    nd_vlist list;
    nd_softkey softkey;
    size_t n;
    size_t i;
    bool picked = false;

    if (ui == NULL || ui->draw == NULL || out == NULL)
        return false;

    /* 256 * sizeof(nd_contact) is ~53 KB; see ND_CONTACTS_PICK_MAX. */
    contacts = malloc(ND_CONTACTS_PICK_MAX * sizeof *contacts);
    names = malloc(ND_CONTACTS_PICK_MAX * sizeof *names);
    if (contacts == NULL || names == NULL) {
        nd_log_err(ND_LOG_UI, "contacts: out of memory for %d rows", (int)ND_CONTACTS_PICK_MAX);
        goto done;
    }

    /* get_all_contacts(search_query). The Python has NO exception handling
     * here: a missing phonebook.db makes sqlite3.connect() create an empty
     * one and the SELECT then raises straight out of the app. nd_contacts_query
     * returns 0 rows instead, which lands in the empty state below --
     * OPEN-QUESTIONS.md PB-4. */
    n = nd_contacts_query(search_query, contacts, ND_CONTACTS_PICK_MAX);

    if (n == 0u) {
        draw_empty(ui, search_query);
        goto done;
    }

    /* Row: (id, name, number, speed_dial) -> index 1 is Name. */
    for (i = 0u; i < n; i++)
        names[i] = contacts[i].name;

    /* VerticalList(ui, title, contact_names, app_id=header_root). app_id is a
     * STRING here -- "1", "1-1", "1-3" -- and nd_vlist_init() only takes an
     * int32_t, so the header is re-initialised from the string afterwards.
     * nd_header owns the only copy of the id, so this is the whole of it. */
    nd_vlist_init(&list, ui, (title != NULL) ? title : ND_CONTACTS_TITLE_DEFAULT,
                  (const char *const *)names, n, 1);
    nd_header_init(&list.header, ui, (header_root != NULL) ? header_root
                                                           : ND_CONTACTS_ROOT_DEFAULT);
    nd_softkey_init(&softkey, ui, false);

    for (;;) {
        int32_t sel;

        /* present=True, unlike the widget gallery's paint-only call: list_ui
         * takes the default, so the strip is pushed to the panel before the
         * list draws over rows 0..145 and presents again. */
        nd_softkey_update(&softkey, (btn_text != NULL) ? btn_text : ND_CONTACTS_BUTTON_DEFAULT,
                          true);

        sel = nd_vlist_show(&list);
        if (sel == ND_WIDGET_BACK)
            break; /* Back pressed -> None */

        if (sel >= 0 && (size_t)sel < n) {
            *out = contacts[sel];
            if (out_index != NULL)
                *out_index = (size_t)sel;
            picked = true;
        }
        break;
    }

done:
    free(names);
    free(contacts);
    return picked;
}

bool nd_contacts_show_selector(nd_ui *ui, const char *title, const char *btn_text,
                               nd_contact *out)
{
    return nd_contacts_pick(ui, title, btn_text, NULL, ND_CONTACTS_ROOT_DEFAULT, out, NULL);
}
