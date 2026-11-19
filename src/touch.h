/*
 * Front touch panel, reduced to one finger's worth of edge events.
 *
 * The panel reports 1920x1088; everything here is scaled down to the 960x544
 * coordinate space the rest of the UI draws in. Only the first finger is
 * tracked, which is all the on-screen keyboard needs -- modifier keys are
 * sticky rather than chorded.
 */
#ifndef VITASSH_TOUCH_H
#define VITASSH_TOUCH_H

#include <stdbool.h>

typedef struct {
	bool down;     /* a finger is on the panel */
	bool pressed;  /* it went down this frame */
	bool released; /* it came up this frame */
	int x, y;      /* current position, in screen coordinates */
} Touch;

void touch_init(void);
void touch_poll(Touch *out);

#endif /* VITASSH_TOUCH_H */
