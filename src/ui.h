/*
 * Screens and the application state machine.
 *
 * main.c owns the frame loop and nothing else; everything about what is on
 * screen and what a button does lives here.
 */
#ifndef VITASSH_UI_H
#define VITASSH_UI_H

#include <stdbool.h>

void ui_init(void);

/* Runs one frame, input through present. False means the user asked to quit. */
bool ui_frame(void);

/*
 * Zeroes the plaintext the session has been holding -- the saved passwords and
 * anything typed into the host form. App is static, so without this they stay
 * in memory until the process goes away.
 */
void ui_shutdown(void);

#endif /* VITASSH_UI_H */
