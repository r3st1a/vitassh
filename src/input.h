/*
 * Controller polling, turned into edge and repeat events.
 *
 * This layer only reports which buttons changed; deciding what a button means
 * is the screen's job, because the d-pad sends cursor keys in the terminal and
 * moves a selection in the host list.
 */
#ifndef VITASSH_INPUT_H
#define VITASSH_INPUT_H

#include <stdbool.h>

enum {
	BTN_UP = 1u << 0,
	BTN_DOWN = 1u << 1,
	BTN_LEFT = 1u << 2,
	BTN_RIGHT = 1u << 3,
	BTN_CROSS = 1u << 4,
	BTN_CIRCLE = 1u << 5,
	BTN_SQUARE = 1u << 6,
	BTN_TRIANGLE = 1u << 7,
	BTN_START = 1u << 8,
	BTN_SELECT = 1u << 9,
	BTN_L = 1u << 10,
	BTN_R = 1u << 11,
};

typedef struct {
	unsigned held;       /* down right now */
	unsigned pressed;    /* went down this frame */
	unsigned repeated;   /* pressed, or auto-repeating while held */
	unsigned released;   /* came up this frame */
	unsigned long_press; /* held past the threshold; fires once */
} Input;

void input_init(void);
void input_poll(Input *in);

#endif /* VITASSH_INPUT_H */
