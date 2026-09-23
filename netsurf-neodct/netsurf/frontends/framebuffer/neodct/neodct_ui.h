/*
 * NeoDCT browser chrome: master input controller.
 *
 * Port of on_key() from the old WebKit browser, driven by the
 * NeoDCT keypad. Pure logic: consumes abstract keys, mutates UI
 * state (mode, menu, cursor, text buffer) and emits at most one
 * browser action per key. The shell renders chrome from the state
 * and executes the actions against the netsurf core.
 */

#ifndef NEODCT_UI_H
#define NEODCT_UI_H

#include <stdbool.h>
#include <stdint.h>

#include "neodct_cursor.h"
#include "neodct_history.h"
#include "neodct_menu.h"

#define NEODCT_TEXT_MAX 511

enum neodct_mode {
	NEODCT_MODE_BROWSE, /**< cursor over the page */
	NEODCT_MODE_MENU,   /**< Options menu overlay */
	NEODCT_MODE_INPUT,  /**< text input popup for a form field */
	NEODCT_MODE_URLBAR, /**< editing the URL bar */
	NEODCT_MODE_HISTORY /**< list of visited pages, from the menu */
};

enum neodct_key {
	NEODCT_KEY_UP,
	NEODCT_KEY_DOWN,
	NEODCT_KEY_LEFT,
	NEODCT_KEY_RIGHT,
	NEODCT_KEY_SELECT, /**< navikey / enter */
	NEODCT_KEY_BACK,   /**< clear key */
	NEODCT_KEY_CHAR    /**< printable character (chr argument) */
};

enum neodct_action_type {
	NEODCT_ACT_NONE,        /**< chrome-only change; just redraw */
	NEODCT_ACT_CLICK,       /**< click page at click.x/click.y */
	NEODCT_ACT_SCROLL,      /**< scroll page by scroll.dx/dy */
	NEODCT_ACT_PASS_KEY,    /**< forward this key to the browser core */
	NEODCT_ACT_NAV_BACK,
	NEODCT_ACT_NAV_FORWARD,
	NEODCT_ACT_NAV_HOME,
	NEODCT_ACT_NAV_RELOAD,
	NEODCT_ACT_NAVIGATE,    /**< go to url in text */
	NEODCT_ACT_COMMIT_TEXT, /**< write text into the focused field */
	NEODCT_ACT_EXIT
};

struct neodct_action {
	enum neodct_action_type type;
	struct { int x, y; } click;
	struct neodct_scroll scroll;
	const char *text; /**< NAVIGATE/COMMIT_TEXT payload */
};

struct neodct_ui {
	enum neodct_mode mode;
	struct neodct_cursor cursor;
	struct neodct_menu menu;
	struct neodct_menu history_menu;
	const struct neodct_history *history; /**< owned by the shell */
	bool hover_editable; /**< cursor is over an editable field */

	char textbuf[NEODCT_TEXT_MAX + 1]; /**< urlbar/input popup text */
	/** textbuf is a prefilled url shown selected: the next character
	 * replaces it and BACK clears it, rather than editing its end */
	bool text_selected;
	/** what "Go to URL" opens with; empty on the home page */
	char page_url[NEODCT_TEXT_MAX + 1];
	char actionbuf[NEODCT_TEXT_MAX + 16]; /**< action text payload */
};

void neodct_ui_init(struct neodct_ui *ui, int width, int height);

/** feed one key event; chr is the codepoint for NEODCT_KEY_CHAR */
void neodct_ui_key(struct neodct_ui *ui, enum neodct_key key,
		   uint32_t chr, struct neodct_action *act);

/** shell reports whether the pointer is over an editable field */
void neodct_ui_set_hover_editable(struct neodct_ui *ui, bool hover);

/** shell opens the input popup preloaded with the field's text */
void neodct_ui_open_input(struct neodct_ui *ui, const char *existing);

/**
 * open the url bar, optionally prefilled; a non-empty prefill starts
 * out selected
 */
void neodct_ui_open_urlbar(struct neodct_ui *ui, const char *prefill);

/** shell reports the url "Go to URL" should offer; NULL or "" for none */
void neodct_ui_set_page_url(struct neodct_ui *ui, const char *url);

/** shell hands over the history the History screen lists */
void neodct_ui_set_history(struct neodct_ui *ui,
			   const struct neodct_history *history);

#endif
