/*
 * VT100/ANSI byte-stream parser feeding the Term grid.
 *
 * Like term.c this file carries no psp2 dependency, so the whole parser can be
 * exercised on the host against recorded escape sequences.
 */
#ifndef VITASSH_VT_H
#define VITASSH_VT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "term.h"

#define VT_MAX_PARAMS 8
#define VT_OUTPUT_CAP 128
#define VT_TITLE_CAP 64

enum {
	VT_GROUND,
	VT_ESC,
	VT_ESC_CHARSET, /* after ESC ( ) * + */
	VT_ESC_HASH,    /* after ESC # */
	VT_CSI,
	VT_CSI_IGNORE,
	VT_OSC,
	VT_OSC_ESC,
	VT_STRING, /* DCS / PM / APC payload: consumed and dropped */
	VT_STRING_ESC,
};

typedef struct {
	int state;

	int params[VT_MAX_PARAMS];
	int nparams;
	int param;
	int have_param;
	int private_marker; /* '?' '<' '>' '=' or 0 */
	int intermediate;

	bool g0_graphics;      /* ESC ( 0 -- DEC special graphics in G0 */
	bool saved_g0_graphics;
	bool app_cursor_keys;  /* DECCKM, ?1 */
	bool bracketed_paste;  /* ?2004 */

	int utf8_need;
	uint32_t utf8_cp;

	char title[VT_TITLE_CAP];
	size_t title_len;

	/* Replies owed to the remote (DSR, DA). The app drains these and writes
	 * them to the channel. */
	char out[VT_OUTPUT_CAP];
	size_t out_len;
	bool out_overflow;
} VtParser;

void vt_init(VtParser *p);

void vt_feed(VtParser *p, Term *t, const uint8_t *data, size_t len);

/* Feeds a NUL-terminated string, for tests and for local echo. */
void vt_feed_str(VtParser *p, Term *t, const char *s);

size_t vt_take_output(VtParser *p, char *dst, size_t cap);

#endif /* VITASSH_VT_H */
