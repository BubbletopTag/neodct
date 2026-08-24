/*
 * NeoDCT browser chrome: master input controller.
 */

#include <string.h>

#include "neodct_ui.h"
#include "neodct_url.h"

/* menu order matches the old browser's Options menu */
static const char *const menu_items[] = {
	"Exit", "Go to URL", "Back", "Forward", "Home", "Reload"
};
#define MENU_COUNT 6
#define MENU_VISIBLE_LINES 3

enum menu_index {
	MENU_EXIT = 0,
	MENU_GO_TO_URL,
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
}

void neodct_ui_set_hover_editable(struct neodct_ui *ui, bool hover)
{
	ui->hover_editable = hover;
}

void neodct_ui_open_input(struct neodct_ui *ui, const char *existing)
{
	ui->mode = NEODCT_MODE_INPUT;
	ui->textbuf[0] = '\0';
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
		ui->mode = NEODCT_MODE_URLBAR;
		ui->textbuf[0] = '\0';
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

static void key_urlbar(struct neodct_ui *ui, enum neodct_key key,
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
	}
}
