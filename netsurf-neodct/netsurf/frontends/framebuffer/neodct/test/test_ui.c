/*
 * Tests for neodct_ui: the master input controller / mode state
 * machine, porting on_key() of the old WebKit browser to the
 * NeoDCT keypad (arrows, SELECT=navikey/enter, BACK=clear).
 *
 * The machine consumes abstract keys and emits at most one browser
 * action per key; the shell reads ui.mode after each event to know
 * what chrome to draw.
 */

#include "test_util.h"
#include "../neodct_ui.h"

int main(void)
{
	struct neodct_ui ui;
	struct neodct_action a;

	/* init: browsing, cursor centred in the 240x175 view */
	neodct_ui_init(&ui, 240, 175);
	CHECK_INT(ui.mode, NEODCT_MODE_BROWSE);
	CHECK_INT(ui.cursor.x, 120);
	CHECK_INT(ui.cursor.y, 87);

	/* --- BROWSE ------------------------------------------------ */

	/* arrows move the cursor, no browser action */
	neodct_ui_key(&ui, NEODCT_KEY_RIGHT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NONE);
	CHECK_INT(ui.cursor.x, 126);

	/* pushing the edge emits a scroll action */
	ui.cursor.x = 239;
	neodct_ui_key(&ui, NEODCT_KEY_RIGHT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_SCROLL);
	CHECK_INT(a.scroll.dx, NEODCT_CURSOR_STEP);
	CHECK_INT(a.scroll.dy, 0);

	/* SELECT clicks at the cursor */
	ui.cursor.x = 50; ui.cursor.y = 60;
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_CLICK);
	CHECK_INT(a.click.x, 50);
	CHECK_INT(a.click.y, 60);

	/* BACK while hovering an editable field is passed through */
	neodct_ui_set_hover_editable(&ui, true);
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_PASS_KEY);
	CHECK_INT(ui.mode, NEODCT_MODE_BROWSE);

	/* BACK otherwise opens the menu, reset to the top */
	neodct_ui_set_hover_editable(&ui, false);
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NONE);
	CHECK_INT(ui.mode, NEODCT_MODE_MENU);
	CHECK_INT(ui.menu.selected, 0);
	CHECK_STR(neodct_menu_selected(&ui.menu), "Exit");

	/* --- MENU -------------------------------------------------- */

	/* BACK closes the menu */
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	CHECK_INT(ui.mode, NEODCT_MODE_BROWSE);
	CHECK_INT(a.type, NEODCT_ACT_NONE);

	/* menu order: Exit, Go to URL, Back, Forward, Home, Reload */
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);   /* reopen */
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	CHECK_STR(neodct_menu_selected(&ui.menu), "Back");
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NAV_BACK);
	CHECK_INT(ui.mode, NEODCT_MODE_BROWSE);

	/* Exit */
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_EXIT);

	/* Forward, Home, Reload */
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	CHECK_STR(neodct_menu_selected(&ui.menu), "Forward");
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NAV_FORWARD);

	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	CHECK_STR(neodct_menu_selected(&ui.menu), "Home");
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NAV_HOME);

	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	for (int i = 0; i < 5; i++)
		neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	CHECK_STR(neodct_menu_selected(&ui.menu), "Reload");
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NAV_RELOAD);

	/* --- URLBAR ------------------------------------------------ */

	/* menu "Go to URL" enters urlbar mode with an empty buffer */
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	CHECK_STR(neodct_menu_selected(&ui.menu), "Go to URL");
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NONE);
	CHECK_INT(ui.mode, NEODCT_MODE_URLBAR);
	CHECK_STR(ui.textbuf, "");

	/* typed characters append */
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, 'f', &a);
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, 'o', &a);
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, 'o', &a);
	CHECK_STR(ui.textbuf, "foo");

	/* BACK deletes one character */
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	CHECK_STR(ui.textbuf, "fo");
	CHECK_INT(ui.mode, NEODCT_MODE_URLBAR);

	/* SELECT normalises and navigates */
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, 'o', &a);
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, '.', &a);
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, 'o', &a);
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, 'r', &a);
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, 'g', &a);
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NAVIGATE);
	CHECK_STR(a.text, "https://foo.org");
	CHECK_INT(ui.mode, NEODCT_MODE_BROWSE);

	/* BACK on an empty urlbar cancels out to browse */
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(ui.mode, NEODCT_MODE_URLBAR);
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	CHECK_INT(ui.mode, NEODCT_MODE_BROWSE);
	CHECK_INT(a.type, NEODCT_ACT_NONE);

	/* SELECT on empty urlbar does nothing (old on_go ignored empty) */
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_DOWN, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NONE);
	CHECK_INT(ui.mode, NEODCT_MODE_URLBAR);
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);

	/* --- INPUT POPUP ------------------------------------------- */

	/* shell opens the popup with the field's existing text */
	neodct_ui_open_input(&ui, "hi");
	CHECK_INT(ui.mode, NEODCT_MODE_INPUT);
	CHECK_STR(ui.textbuf, "hi");

	/* typing appends; SELECT commits */
	neodct_ui_key(&ui, NEODCT_KEY_CHAR, '!', &a);
	neodct_ui_key(&ui, NEODCT_KEY_SELECT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_COMMIT_TEXT);
	CHECK_STR(a.text, "hi!");
	CHECK_INT(ui.mode, NEODCT_MODE_BROWSE);

	/* BACK deletes; BACK on empty cancels without committing */
	neodct_ui_open_input(&ui, "x");
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	CHECK_STR(ui.textbuf, "");
	CHECK_INT(ui.mode, NEODCT_MODE_INPUT);
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NONE);
	CHECK_INT(ui.mode, NEODCT_MODE_BROWSE);

	/* arrows are ignored while the popup is open */
	neodct_ui_open_input(&ui, "");
	ui.cursor.x = 120;
	neodct_ui_key(&ui, NEODCT_KEY_LEFT, 0, &a);
	CHECK_INT(a.type, NEODCT_ACT_NONE);
	CHECK_INT(ui.cursor.x, 120);
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);

	/* clicking the url bar opens it prefilled with the current url */
	neodct_ui_open_urlbar(&ui, "https://foo.org");
	CHECK_INT(ui.mode, NEODCT_MODE_URLBAR);
	CHECK_STR(ui.textbuf, "https://foo.org");
	neodct_ui_key(&ui, NEODCT_KEY_BACK, 0, &a);
	CHECK_STR(ui.textbuf, "https://foo.or");

	/* NULL prefill opens it empty */
	neodct_ui_open_urlbar(&ui, NULL);
	CHECK_STR(ui.textbuf, "");

	TEST_EXIT();
}
