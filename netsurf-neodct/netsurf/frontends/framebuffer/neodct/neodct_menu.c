/*
 * NeoDCT browser chrome: vertical list menu state.
 */

#include <stddef.h>

#include "neodct_menu.h"

void neodct_menu_init(struct neodct_menu *m, const char *const *items,
		      int count, int max_lines)
{
	m->items = items;
	m->count = count;
	m->max_lines = max_lines;
	neodct_menu_reset(m);
}

void neodct_menu_reset(struct neodct_menu *m)
{
	m->selected = 0;
	m->window_start = 0;
}

void neodct_menu_up(struct neodct_menu *m)
{
	if (m->selected <= 0)
		return;
	m->selected--;
	if (m->selected < m->window_start)
		m->window_start--;
}

void neodct_menu_down(struct neodct_menu *m)
{
	if (m->selected >= m->count - 1)
		return;
	m->selected++;
	if (m->selected >= m->window_start + m->max_lines)
		m->window_start++;
}

const char *neodct_menu_selected(const struct neodct_menu *m)
{
	if (m->selected < 0 || m->selected >= m->count)
		return NULL;
	return m->items[m->selected];
}
