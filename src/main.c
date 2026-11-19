/*
 * Entry point: bring the subsystems up, then hand the frame loop to ui.c.
 *
 * vita2d_swap_buffers() queues the frame and returns immediately -- it does not
 * wait for vblank (the disassembly is four instructions and no wait), so the
 * pacing here is what keeps the loop at 60 Hz instead of spinning. That also
 * throttles the libssh2 state machine, which is driven from the same loop.
 */
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>

#include "ime.h"
#include "input.h"
#include "log.h"
#include "net.h"
#include "render.h"
#include "touch.h"
#include "ui.h"

int main(void)
{
	log_open();
	log_printf("vitassh: starting");

	if (render_init() < 0) {
		/* Nothing can be shown, so the log is the only place this can go. */
		log_printf("vitassh: render_init failed, giving up");
		log_close();
		return 1;
	}

	ime_init();
	input_init();
	touch_init();

	if (net_init() != NET_OK)
		log_printf("vitassh: networking is unavailable");

	ui_init();
	log_printf("vitassh: entering the main loop");

	while (ui_frame())
		sceDisplayWaitVblankStart();

	log_printf("vitassh: shutting down");
	ui_shutdown();
	net_term();
	render_shutdown();
	log_close();

	sceKernelExitProcess(0);
	return 0;
}
