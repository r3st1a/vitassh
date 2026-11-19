/*
 * The on-screen terminal keyboard.
 *
 * A plain QWERTY overlay with a function bar, drawn by us instead of raised as
 * the system IME dialog: keys go straight down the SSH channel, so there is no
 * second screen to enter and leave between commands. Ctrl, Alt and Shift are
 * sticky (a tap arms them for the next key), which is what makes Ctrl-C possible
 * without a chord on a touch panel.
 */
#ifndef VITASSH_KEYBOARD_H
#define VITASSH_KEYBOARD_H

#include <stdbool.h>
#include <stdint.h>

#include "touch.h"

/*
 * Overlay geometry: the bottom of the 960x544 screen. KB_Y is a multiple of
 * CELL_H so the terminal can slide up by whole rows and a line boundary lands
 * on the top edge of the keyboard.
 */
#define KB_Y 260
#define KB_H 284

enum {
	KB_PAGE_ALPHA,
	KB_PAGE_NUM,
	KB_PAGE_SYM,
};

typedef struct {
	bool visible;
	int page;

	bool shift;
	bool shift_lock;
	bool ctrl;
	bool alt;

	/* Key under the finger, for the pressed highlight. -1 when none. */
	int active_fn;
	int active_row;
	int active_col;

	/* Held key state for Backspace and the arrow keys. */
	int held_fn;
	int held_row;
	int held_col;
	int held_action;
	uint64_t repeat_at;
} Kb;

void kb_init(Kb *kb);
void kb_show(Kb *kb, bool show);
bool kb_visible(const Kb *kb);

/* Draws the whole overlay. */
void kb_draw(const Kb *kb);

/*
 * Consumes one frame of touch input and writes terminal bytes to `out`.
 * Returns the number of bytes written, 0 when nothing was sent, or -1 when the
 * hide key was pressed. `cursor_keys` mirrors DECCKM so arrows match what the
 * remote's terminfo expects.
 */
int kb_input(Kb *kb, const Touch *touch, bool cursor_keys, char *out, int cap);

#endif /* VITASSH_KEYBOARD_H */
