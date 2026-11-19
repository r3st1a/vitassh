#include "touch.h"

#include <psp2/touch.h>

#include <string.h>

/* The panel is 1920x1088; the UI is 960x544. */
#define PANEL_SCALE 2

static bool tracking;
static unsigned tracking_id;
static int last_x, last_y;

void touch_init(void)
{
	tracking = false;
	tracking_id = 0;
	last_x = 0;
	last_y = 0;

	sceTouchSetSamplingState(SCE_TOUCH_PORT_FRONT,
	                         SCE_TOUCH_SAMPLING_STATE_START);
}

void touch_poll(Touch *out)
{
	SceTouchData data;
	bool found = false;
	unsigned i;

	memset(out, 0, sizeof(*out));
	memset(&data, 0, sizeof(data));

	if (sceTouchPeek(SCE_TOUCH_PORT_FRONT, &data, 1) < 0) return;

	for (i = 0; i < data.reportNum && i < SCE_TOUCH_MAX_REPORT; i++) {
		if (!tracking || data.report[i].id == tracking_id) {
			found = true;
			tracking_id = data.report[i].id;
			last_x = data.report[i].x / PANEL_SCALE;
			last_y = data.report[i].y / PANEL_SCALE;
			break;
		}
	}

	if (!tracking && found) {
		tracking = true;
		out->pressed = true;
	} else if (tracking && !found) {
		tracking = false;
		out->released = true;
	}

	out->down = tracking;
	out->x = last_x;
	out->y = last_y;
}
