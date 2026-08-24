/*
 * NeoDCT browser chrome: vertical list menu state.
 *
 * Pure logic, no netsurf dependencies, unit tested in test/.
 * Rendering walks items[window_start .. window_start+max_lines-1]
 * and highlights the row where index == selected.
 */

#ifndef NEODCT_MENU_H
#define NEODCT_MENU_H

struct neodct_menu {
	const char *const *items;
	int count;
	int max_lines;    /**< visible rows */
	int selected;     /**< index of the highlighted item */
	int window_start; /**< index of the first visible item */
};

void neodct_menu_init(struct neodct_menu *m, const char *const *items,
		      int count, int max_lines);

/** reset selection/window to the top (menu reopened) */
void neodct_menu_reset(struct neodct_menu *m);

void neodct_menu_up(struct neodct_menu *m);
void neodct_menu_down(struct neodct_menu *m);

/** currently highlighted item text */
const char *neodct_menu_selected(const struct neodct_menu *m);

#endif
