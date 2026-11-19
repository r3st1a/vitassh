#include "input.h"

#include <psp2/ctrl.h>

#include <string.h>

#include "clock.h"

#define REPEAT_DELAY_MS 400 /* hold this long before repeating starts */
#define REPEAT_RATE_MS 70
#define LONG_PRESS_MS 600

#define BUTTON_COUNT 12

typedef struct {
	unsigned bit;
	unsigned sce;
	bool repeat;
} ButtonMap;

static const ButtonMap buttons[BUTTON_COUNT] = {
	{BTN_UP, SCE_CTRL_UP, true},
	{BTN_DOWN, SCE_CTRL_DOWN, true},
	{BTN_LEFT, SCE_CTRL_LEFT, true},
	{BTN_RIGHT, SCE_CTRL_RIGHT, true},
	{BTN_CROSS, SCE_CTRL_CROSS, false},
	{BTN_CIRCLE, SCE_CTRL_CIRCLE, false},
	{BTN_SQUARE, SCE_CTRL_SQUARE, false},
	{BTN_TRIANGLE, SCE_CTRL_TRIANGLE, false},
	{BTN_START, SCE_CTRL_START, false},
	{BTN_SELECT, SCE_CTRL_SELECT, false},
	{BTN_L, SCE_CTRL_LTRIGGER, false},
	{BTN_R, SCE_CTRL_RTRIGGER, false},
};

static unsigned previous;
static uint64_t down_since[BUTTON_COUNT];
static uint64_t repeat_at[BUTTON_COUNT];
static bool long_fired[BUTTON_COUNT];

void input_init(void)
{
	memset(down_since, 0, sizeof(down_since));
	memset(repeat_at, 0, sizeof(repeat_at));
	memset(long_fired, 0, sizeof(long_fired));
	previous = 0;

	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
}

void input_poll(Input *in)
{
	SceCtrlData pad;
	unsigned held = 0;
	uint64_t now;
	int i;

	memset(in, 0, sizeof(*in));

	if (sceCtrlPeekBufferPositive(0, &pad, 1) < 0) return;

	for (i = 0; i < BUTTON_COUNT; i++) {
		if (pad.buttons & buttons[i].sce) held |= buttons[i].bit;
	}

	now = now_ms();

	in->pressed = held & ~previous;
	in->released = previous & ~held;
	in->held = held;

	for (i = 0; i < BUTTON_COUNT; i++) {
		unsigned bit = buttons[i].bit;

		if (in->pressed & bit) {
			down_since[i] = now;
			repeat_at[i] = now + REPEAT_DELAY_MS;
			long_fired[i] = false;
			if (buttons[i].repeat) in->repeated |= bit;
			continue;
		}

		if (!(held & bit)) continue;

		if (!long_fired[i] && now - down_since[i] >= LONG_PRESS_MS) {
			long_fired[i] = true;
			in->long_press |= bit;
		}

		if (buttons[i].repeat && now >= repeat_at[i]) {
			repeat_at[i] = now + REPEAT_RATE_MS;
			in->repeated |= bit;
		}
	}

	previous = held;
}
