/*
 * NeoDCT browser chrome: master input controller.
 */

#include <string.h>

#include "neodct_ui.h"
#include "neodct_url.h"

/* menu order matches the old browser's Options menu, with History
 * beside Go to URL: both are ways of choosing where to go next */
static const char *const menu_items[] = {
	"Exit", "Go to URL", "History", "Back", "Forward", "Home", "Reload"
};
#define MENU_COUNT 7
#define MENU_VISIBLE_LINES 3

enum menu_index {
	MENU_EXIT = 0,
	MENU_GO_TO_URL,
	MENU_HISTORY,
	MENU_BACK,
	MENU_FORWARD,
	MENU_HOME,
	MENU_RELOAD
};

void neodct_ui_init(struct neodct_ui *ui, int width, int height)
{
	memset(ui, 0, sizeof(*ui));
	ui->mode = NEODCT_MODE_BROWSE;
	neodct_cursor_init(&ui->cursor, width, height);
	neodct_menu_init(&ui->menu, menu_items, MENU_COUNT,
			 MENU_VISIBLE_LINES);
	neodct_menu_init(&ui->history_menu, NULL, 0, MENU_VISIBLE_LINES);
}

void neodct_ui_set_page_url(struct neodct_ui *ui, const char *url)
{
	ui->page_url[0] = '\0';
	if (url != NULL) {
		strncpy(ui->page_url, url, NEODCT_TEXT_MAX);
		ui->page_url[NEODCT_TEXT_MAX] = '\0';
	}
}

void neodct_ui_set_history(struct neodct_ui *ui,
			   const struct neodct_history *history)
{
	ui->history = history;
}

void neodct_ui_set_hover_editable(struct neodct_ui *ui, bool hover)
{
	ui->hover_editable = hover;
}

void neodct_ui_open_input(struct neodct_ui *ui, const char *existing)
{
	ui->mode = NEODCT_MODE_INPUT;
	ui->textbuf[0] = '\0';
	ui->text_selected = false;
	if (existing != NULL) {
		strncpy(ui->textbuf, existing, NEODCT_TEXT_MAX);
		ui->textbuf[NEODCT_TEXT_MAX] = '\0';
	}
}

void neodct_ui_open_urlbar(struct neodct_ui *ui, const char *prefill)
{
	ui->mode = NEODCT_MODE_URLBAR;
	ui->textbuf[0] = '\0';
	if (prefill != NULL) {
		strncpy(ui->textbuf, prefill, NEODCT_TEXT_MAX);
		ui->textbuf[NEODCT_TEXT_MAX] = '\0';
	}
	/* Selected, so that a new address costs no more key presses than
	 * it did when the bar opened empty: the first letter typed
	 * replaces the whole url, and C clears it in one press instead of
	 * one press per character. */
	ui->text_selected = (ui->textbuf[0] != '\0');
}

static void open_history(struct neodct_ui *ui)
{
	ui->mode = NEODCT_MODE_HISTORY;
	if (ui->history != NULL)
		neodct_menu_init(&ui->history_menu, ui->history->labels,
				 ui->history->count, MENU_VISIBLE_LINES);
	else
		neodct_menu_init(&ui->history_menu, NULL, 0,
				 MENU_VISIBLE_LINES);
}

static void textbuf_append(struct neodct_ui *ui, uint32_t chr)
{
	size_t len = strlen(ui->textbuf);

	/* ASCII for now; keypad input never produces more */
	if (chr < 0x20 || chr > 0x7e)
		return;
	if (len >= NEODCT_TEXT_MAX)
		return;
	ui->textbuf[len] = (char)chr;
	ui->textbuf[len + 1] = '\0';
}

static void textbuf_delete(struct neodct_ui *ui)
{
	size_t len = strlen(ui->textbuf);

	if (len > 0)
		ui->textbuf[len - 1] = '\0';
}

static void menu_select(struct neodct_ui *ui, struct neodct_action *act)
{
	int item = ui->menu.selected;

	/* menu closes on any selection, like the old toggle_menu() */
	ui->mode = NEODCT_MODE_BROWSE;

	switch (item) {
	case MENU_EXIT:
		act->type = NEODCT_ACT_EXIT;
		break;
	case MENU_GO_TO_URL:
		neodct_ui_open_urlbar(ui, ui->page_url);
		break;
	case MENU_HISTORY:
		open_history(ui);
		break;
	case MENU_BACK:
		act->type = NEODCT_ACT_NAV_BACK;
		break;
	case MENU_FORWARD:
		act->type = NEODCT_ACT_NAV_FORWARD;
		break;
	case MENU_HOME:
		act->type = NEODCT_ACT_NAV_HOME;
		break;
	case MENU_RELOAD:
		act->type = NEODCT_ACT_NAV_RELOAD;
		break;
	}
}

static void key_browse(struct neodct_ui *ui, enum neodct_key key,
		       struct neodct_action *act)
{
	switch (key) {
	case NEODCT_KEY_LEFT:
	case NEODCT_KEY_RIGHT:
	case NEODCT_KEY_UP:
	case NEODCT_KEY_DOWN: {
		enum neodct_dir dir =
			(key == NEODCT_KEY_LEFT) ? NEODCT_DIR_LEFT :
			(key == NEODCT_KEY_RIGHT) ? NEODCT_DIR_RIGHT :
			(key == NEODCT_KEY_UP) ? NEODCT_DIR_UP :
			NEODCT_DIR_DOWN;

		neodct_cursor_move(&ui->cursor, dir, NEODCT_CURSOR_STEP,
				   &act->scroll);
		if (act->scroll.dx != 0 || act->scroll.dy != 0)
			act->type = NEODCT_ACT_SCROLL;
		break;
	}
	case NEODCT_KEY_SELECT:
		act->type = NEODCT_ACT_CLICK;
		act->click.x = ui->cursor.x;
		act->click.y = ui->cursor.y;
		break;
	case NEODCT_KEY_BACK:
		if (ui->hover_editable) {
			/* deleting text in a page field */
			act->type = NEODCT_ACT_PASS_KEY;
		} else {
			ui->mode = NEODCT_MODE_MENU;
			neodct_menu_reset(&ui->menu);
		}
		break;
	case NEODCT_KEY_CHAR:
		act->type = NEODCT_ACT_PASS_KEY;
		break;
	}
}

static void key_menu(struct neodct_ui *ui, enum neodct_key key,
		     struct neodct_action *act)
{
	switch (key) {
	case NEODCT_KEY_UP:
		neodct_menu_up(&ui->menu);
		break;
	case NEODCT_KEY_DOWN:
		neodct_menu_down(&ui->menu);
		break;
	case NEODCT_KEY_SELECT:
		menu_select(ui, act);
		break;
	case NEODCT_KEY_BACK:
		ui->mode = NEODCT_MODE_BROWSE;
		break;
	default:
		break;
	}
}

static void key_history(struct neodct_ui *ui, enum neodct_key key,
			struct neodct_action *act)
{
	const char *url;

	switch (key) {
	case NEODCT_KEY_UP:
		neodct_menu_up(&ui->history_menu);
		break;
	case NEODCT_KEY_DOWN:
		neodct_menu_down(&ui->history_menu);
		break;
	case NEODCT_KEY_SELECT:
		url = (ui->history != NULL) ?
			neodct_history_url(ui->history,
					   ui->history_menu.selected) : NULL;
		if (url == NULL) {
			/* nothing to choose: the softkey says Back */
			ui->mode = NEODCT_MODE_MENU;
			break;
		}
		strncpy(ui->actionbuf, url, sizeof(ui->actionbuf) - 1);
		ui->actionbuf[sizeof(ui->actionbuf) - 1] = '\0';
		act->type = NEODCT_ACT_NAVIGATE;
		act->text = ui->actionbuf;
		ui->mode = NEODCT_MODE_BROWSE;
		break;
	case NEODCT_KEY_BACK:
		/* one level up, to the Options menu it was opened from */
		ui->mode = NEODCT_MODE_MENU;
		break;
	default:
		break;
	}
}

static void key_urlbar(struct neodct_ui *ui, enum neodct_key key,
		       uint32_t chr, struct neodct_action *act)
{
	switch (key) {
	case NEODCT_KEY_CHAR:
		if (ui->text_selected) {
			ui->textbuf[0] = '\0';
			ui->text_selected = false;
		}
		textbuf_append(ui, chr);
		break;
	case NEODCT_KEY_BACK:
		if (ui->text_selected) {
			ui->textbuf[0] = '\0';
			ui->text_selected = false;
		} else if (ui->textbuf[0] == '\0') {
			ui->mode = NEODCT_MODE_BROWSE;
		} else {
			textbuf_delete(ui);
		}
		break;
	case NEODCT_KEY_UP:
	case NEODCT_KEY_DOWN:
	case NEODCT_KEY_LEFT:
	case NEODCT_KEY_RIGHT:
		/* keep the url and edit its end instead */
		ui->text_selected = false;
		break;
	case NEODCT_KEY_SELECT:
		if (neodct_url_normalize(ui->textbuf, ui->actionbuf,
					 sizeof(ui->actionbuf)) != NULL) {
			act->type = NEODCT_ACT_NAVIGATE;
			act->text = ui->actionbuf;
			ui->mode = NEODCT_MODE_BROWSE;
		}
		/* empty url: stay in the urlbar, like the old on_go */
		break;
	default:
		break;
	}
}

static void key_input(struct neodct_ui *ui, enum neodct_key key,
		      uint32_t chr, struct neodct_action *act)
{
	switch (key) {
	case NEODCT_KEY_CHAR:
		textbuf_append(ui, chr);
		break;
	case NEODCT_KEY_BACK:
		if (ui->textbuf[0] == '\0')
			ui->mode = NEODCT_MODE_BROWSE;
		else
			textbuf_delete(ui);
		break;
	case NEODCT_KEY_SELECT:
		strcpy(ui->actionbuf, ui->textbuf);
		act->type = NEODCT_ACT_COMMIT_TEXT;
		act->text = ui->actionbuf;
		ui->mode = NEODCT_MODE_BROWSE;
		break;
	default:
		break;
	}
}

void neodct_ui_key(struct neodct_ui *ui, enum neodct_key key,
		   uint32_t chr, struct neodct_action *act)
{
	memset(act, 0, sizeof(*act));
	act->type = NEODCT_ACT_NONE;

	switch (ui->mode) {
	case NEODCT_MODE_BROWSE:
		key_browse(ui, key, act);
		break;
	case NEODCT_MODE_MENU:
		key_menu(ui, key, act);
		break;
	case NEODCT_MODE_URLBAR:
		key_urlbar(ui, key, chr, act);
		break;
	case NEODCT_MODE_INPUT:
		key_input(ui, key, chr, act);
		break;
	case NEODCT_MODE_HISTORY:
		key_history(ui, key, act);
		break;
	}
}
