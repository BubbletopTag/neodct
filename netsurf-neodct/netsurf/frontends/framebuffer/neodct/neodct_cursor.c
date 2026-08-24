/*
 * NeoDCT browser chrome: keypad-driven virtual pointer.
 */

#include "neodct_cursor.h"

void neodct_cursor_init(struct neodct_cursor *c, int width, int height)
{
	c->width = width;
	c->height = height;
	c->x = width / 2;
	c->y = height / 2;
}

void neodct_cursor_move(struct neodct_cursor *c, enum neodct_dir dir,
			int step, struct neodct_scroll *scroll)
{
	int target;

	scroll->dx = 0;
	scroll->dy = 0;

	switch (dir) {
	case NEODCT_DIR_LEFT:
		target = c->x - step;
		if (target < 0) {
			c->x = 0;
			scroll->dx = -step;
		} else {
			c->x = target;
		}
		break;
	case NEODCT_DIR_RIGHT:
		target = c->x + step;
		if (target >= c->width) {
			c->x = c->width - 1;
			scroll->dx = step;
		} else {
			c->x = target;
		}
		break;
	case NEODCT_DIR_UP:
		target = c->y - step;
		if (target < 0) {
			c->y = 0;
			scroll->dy = -step;
		} else {
			c->y = target;
		}
		break;
	case NEODCT_DIR_DOWN:
		target = c->y + step;
		if (target >= c->height) {
			c->y = c->height - 1;
			scroll->dy = step;
		} else {
			c->y = target;
		}
		break;
	}
}
