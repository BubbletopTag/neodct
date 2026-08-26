/* phonebook.h -- the pieces of the PhoneBook app a unit test can reach.
 *
 * System/apps/PhoneBook/main.py is 214 lines of module-level functions:
 * _draw_center_message, add_entry_action, edit_contact_action,
 * delete_contact_action, run_contact_options, run_options_submenu and run.
 * The three that touch the database and the two that draw a screen by hand
 * are the whole part of this app that is not "call a widget", so they are
 * declared here rather than left static inside main.c.
 *
 * test/unit/test_phonebook.c dlopen()s the BUILT app.so and dlsym()s them,
 * the way test_cubebench.c does, so the test exercises the artefact that
 * ships and not a second copy compiled with different flags.
 *
 * Names follow CODING-STANDARDS.md section 2; the Python name each one came
 * from is on its declaration.
 */

#ifndef ND_PHONEBOOK_H_INCLUDED
#define ND_PHONEBOOK_H_INCLUDED

#include "nd_db.h"
#include "nd_font.h"
#include "nd_types.h"
#include "nd_ui.h"

#ifdef __cplusplus
extern "C" {
#endif

/* App id 1 -- manifest.json, and the VerticalList's app_id. */
#define ND_PHONEBOOK_APP_ID 1

/* The main menu, in the Python's order. Index 4 ("Send entry") and index 6
 * ("1-touch dialing") fall off the end of run()'s if/elif chain and DO
 * NOTHING: there is no branch for either. They are on screen because they are
 * on screen today. */
#define ND_PHONEBOOK_MAIN_ITEMS 7
extern const char *const nd_phonebook_main_items[ND_PHONEBOOK_MAIN_ITEMS];

/* The Options submenu, app_id "1-6". Both entries print a line to the serial
 * console and draw nothing at all. */
#define ND_PHONEBOOK_OPT_ITEMS 2
extern const char *const nd_phonebook_opt_items[ND_PHONEBOOK_OPT_ITEMS];

/* run_contact_options()' four entries. */
#define ND_PHONEBOOK_CONTACT_ITEMS 4
extern const char *const nd_phonebook_contact_items[ND_PHONEBOOK_CONTACT_ITEMS];

/* ------------------------------------------------------------------ *
 * The database actions (pb_db.c)
 * ------------------------------------------------------------------ *
 *
 * All three open /NeoDCT/User/db/phonebook.db, run one statement, commit and
 * close -- nd_db.h's connection policy, and the Python's. None of them draws;
 * the caller owns the "Saved!" / "Updated!" / "Erased" message, because in
 * the Python that message is inside the try block and therefore does not
 * appear when the statement raises.
 */

/* `err`/`err_sz` receive sqlite's own message on failure, so the caller can
 * print the "[PB] Save Error: <e>" line the Python printed with the exception
 * text in it. Pass NULL to discard it. On success the buffer is set to "". */
#define ND_PHONEBOOK_ERR_MAX 160

/* INSERT INTO contacts (name, number, speed_dial) VALUES (?, ?, 0) */
nd_err nd_phonebook_insert(const char *name, const char *number, char *err, size_t err_sz);

/* UPDATE contacts SET name=?, number=? WHERE id=? */
nd_err nd_phonebook_update(int64_t id, const char *name, const char *number, char *err,
                           size_t err_sz);

/* DELETE FROM contacts WHERE id=? -- the Python has NO try/except around this
 * one, so a failure there propagates out of the app and reaches the crash
 * screen. The C logs it and carries on instead; OPEN-QUESTIONS.md PB-6. */
nd_err nd_phonebook_delete(int64_t id, char *err, size_t err_sz);

/* ------------------------------------------------------------------ *
 * The two hand-drawn screens (main.c)
 * ------------------------------------------------------------------ */

/* _draw_center_message(ui, text, duration, font, fill). font NULL means
 * ui->font_xl, which is the Python's `font or ui.font_xl`. Clears rows
 * 0..content_bottom, centres the ink box, presents, then holds the screen for
 * `duration` seconds. */
void nd_phonebook_center_message(nd_ui *ui, const char *text, double duration, const nd_font *font,
                                 nd_color fill);

/* run_contact_options() item 0, "Call".
 *
 * IT DOES NOT DIAL. It paints three lines and sleeps for two seconds; the
 * modem is never touched, and README.md line 144 has said so since 0.3.0
 * ("Phonebook (SQLite-backed; calling action is buggy)"). Ported as-is;
 * OPEN-QUESTIONS.md PB-5 describes the bug. */
void nd_phonebook_calling_screen(nd_ui *ui, const nd_contact *contact);

/* "1-1-%d" % (selection_index + 1) -- the breadcrumb run_contact_options()
 * gets after a Search hit. Truncation is reported, never silently trimmed. */
nd_err nd_phonebook_options_root(char *out, size_t out_sz, size_t selection_index);

#ifdef __cplusplus
}
#endif

#endif /* ND_PHONEBOOK_H_INCLUDED */
