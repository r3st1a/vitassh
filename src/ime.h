/*
 * On-screen keyboard (SceImeDialog).
 *
 * The dialog is a system overlay, so it only appears if the frame is handed to
 * the common dialog layer at the right moment -- see render_dialog_update() and
 * the ordering note in main.c.
 */
#ifndef VITASSH_IME_H
#define VITASSH_IME_H

#include <stdbool.h>

typedef enum {
	IME_IDLE,     /* no dialog on screen */
	IME_ACTIVE,   /* the user is typing */
	IME_ACCEPTED, /* ime_text() holds the result */
	IME_CANCELLED,
} ImeStatus;

/*
 * Which keyboard to raise. Kept as a project enum so the SDK's SceImeType and
 * option bits stay inside ime.c.
 */
typedef enum {
	IME_KIND_TEXT,   /* shell commands, display names: candidates allowed */
	IME_KIND_HOST,   /* host, address or user name: no autocaps, no candidates */
	IME_KIND_NUMBER, /* port: numeric keypad */
} ImeKind;

/* One-time setup: sceAppUtilInit and the common dialog configuration. */
int ime_init(void);

/*
 * Queues the keyboard; it appears on the next frame. `password` hides the text
 * as it is typed. Returns 0 on success, -1 if the dialog could not be created
 * (usually because one is already open).
 */
int ime_open(const char *title, const char *initial, int max_len, ImeKind kind,
             bool password);

/* Polls the dialog and advances the status. Safe to call every frame. */
ImeStatus ime_poll(void);

/* UTF-8 result; valid once ime_poll() has returned IME_ACCEPTED. */
const char *ime_text(void);

#endif /* VITASSH_IME_H */
