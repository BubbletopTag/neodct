/*
 * NeoDCT browser chrome: keypad-driven virtual pointer.
 *
 * Arrow keys step the cursor; pushing against a view edge emits a
 * page scroll instead. Pure logic, unit tested in test/.
 */

#ifndef NEODCT_CURSOR_H
#define NEODCT_CURSOR_H

#define NEODCT_CURSOR_STEP      6
#define NEODCT_CURSOR_STEP_FAST 14

enum neodct_dir {
	NEODCT_DIR_LEFT,
	NEODCT_DIR_RIGHT,
	NEODCT_DIR_UP,
	NEODCT_DIR_DOWN
};

struct neodct_cursor {
	int x, y;
	int width, height; /**< view bounds the cursor lives in */
};

/** scroll request emitted by a cursor move (0,0 when none needed) */
struct neodct_scroll {
	int dx, dy;
};

/** place the cursor at the view centre */
void neodct_cursor_init(struct neodct_cursor *c, int width, int height);

/**
 * Step the cursor; when the step would leave the view, clamp to the
 * edge and emit a full-step scroll in that direction instead.
 */
void neodct_cursor_move(struct neodct_cursor *c, enum neodct_dir dir,
			int step, struct neodct_scroll *scroll);

#endif
