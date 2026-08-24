/* nd_contacts.h -- the shared contact picker.
 *
 * ADDITION to the frozen header set, recorded in OPEN-QUESTIONS.md as PB-1.
 * OPEN-QUESTIONS.md U-4 already noted the gap: nd_ui.c declares and weakly
 * references nd_contacts_show_selector() so the home screen's Up/Down key
 * does nothing until somebody defines it, and nothing in include/ named it.
 * This is that name, spelled exactly as nd_ui.c already spells it, plus the
 * full-argument form the apps need.
 *
 * ============ WHY THIS LIVES IN libneodct AND NOT IN PhoneBook ============
 *
 * System/apps/PhoneBook/shared/list_ui.py has THREE importers in two
 * different processes:
 *
 *   System/core/main.py handle_input()  Up/Down on the home screen opens it
 *                                       with title="Select", btn_text="Call"
 *   System/apps/PhoneBook/main.py       Search / Edit / Erase
 *   System/apps/Messages/main.py        ContactNumberInput
 *
 * The core is nd-core. It fork()s and execve()s nd-apprun and never dlopen()s
 * an app.so itself, so a definition inside apps/PhoneBook/app.so is a symbol
 * the core process can never reach. The only place all three callers can see
 * one copy is the shared library, which is also what spec-apps-core.md
 * section C2 concluded. Hence lib/nd_contacts.c.
 *
 * ============ THE ROW TYPE IS nd_db.h's ============
 *
 * The Python passes the raw sqlite tuple around and every consumer indexes it
 * positionally as (id, name, number, speed_dial). nd_db.h already declares
 * that shape as nd_contact and already owns the query, so nothing new is
 * introduced here.
 */

#ifndef ND_CONTACTS_H_INCLUDED
#define ND_CONTACTS_H_INCLUDED

#include "nd_db.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* get_all_contacts() returns an unbounded list and show_contact_selector()
 * puts every row on a VerticalList. CODING-STANDARDS.md section 1.5 will not
 * have an array sized by the database, so the picker reads at most this many
 * rows and shows those. 256 * sizeof(nd_contact) is ~53 KB, taken from the
 * heap for the life of one picker and released before it returns. A SIM holds
 * 250 contacts, so the cap is above anything the phone can actually reach by
 * importing. Recorded in OPEN-QUESTIONS.md as PB-2. */
#define ND_CONTACTS_PICK_MAX 256

/* show_contact_selector()'s Python default arguments. Passing NULL for any of
 * title, btn_text or header_root selects the default, which is C's spelling
 * of leaving the argument out. */
#define ND_CONTACTS_TITLE_DEFAULT  "Contacts"
#define ND_CONTACTS_BUTTON_DEFAULT "Select"
#define ND_CONTACTS_ROOT_DEFAULT   "1"

/* list_ui.show_contact_selector(ui, title, btn_text, search_query,
 * header_root), whole.
 *
 * Queries the contacts table (ORDER BY name ASC, optionally LIKE '%q%'),
 * shows a VerticalList of the names with `btn_text` on the softkey, and
 * blocks until a row is chosen or Clear is pressed.
 *
 *   true   a row was chosen. *out is that row; *out_index, when non-NULL, is
 *          its zero-based position in the list -- PhoneBook builds the
 *          "1-1-<n>" breadcrumb out of it.
 *   false  Clear was pressed, or there was nothing to show. In the empty
 *          case the picker paints "No Results" (with a query) or
 *          "No Contacts" (without one) centred, holds it for 1.5 s, and
 *          returns without ever offering a list. *out is untouched either
 *          way.
 *
 * search_query NULL or "" both mean "everything", exactly as Python's falsy
 * test does. */
bool nd_contacts_pick(nd_ui *ui, const char *title, const char *btn_text, const char *search_query,
                      const char *header_root, nd_contact *out, size_t *out_index);

/* The core's spelling, already referenced (weakly) from nd_ui.c:
 * nd_contacts_pick(ui, title, btn_text, NULL, "1", out, NULL). */
bool nd_contacts_show_selector(nd_ui *ui, const char *title, const char *btn_text, nd_contact *out);

#ifdef __cplusplus
}
#endif

#endif /* ND_CONTACTS_H_INCLUDED */
