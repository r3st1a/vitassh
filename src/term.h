/*
 * Terminal grid model: cells, scrollback, cursor, scroll region.
 *
 * Deliberately free of any psp2 header so this file (and vt.c, config.c) can be
 * compiled on the host and unit tested without a Vita.
 */
#ifndef VITASSH_TERM_H
#define VITASSH_TERM_H

#include <stdbool.h>
#include <stdint.h>

/* 960x544 screen, 8x8 glyphs drawn 1:1 in an 8x10 cell.
 * 120x51 fills 960x510, leaving 34px for the status bar. */
#define TERM_COLS 120
#define TERM_ROWS 51
#define TERM_SB_LINES 512

/* Colour indices 0-15 are the ANSI palette; -1 means "terminal default". */
#define TERM_COLOR_DEFAULT (-1)
#define TERM_PALETTE_SIZE 16

/*
 * Rendition bits. BOLD/DIM/REVERSE/INVISIBLE are resolved into the cell's
 * effective colours when the cell is written, so a stored cell only ever
 * carries UNDERLINE -- that is the one we can still show with a bitmap font.
 */
enum {
	TERM_ATTR_BOLD = 1u << 0,
	TERM_ATTR_DIM = 1u << 1,
	TERM_ATTR_UNDERLINE = 1u << 2,
	TERM_ATTR_REVERSE = 1u << 3,
	TERM_ATTR_INVISIBLE = 1u << 4,
};

typedef struct {
	uint32_t ch;
	int8_t fg;
	int8_t bg;
	uint8_t attr;
} TermCell;

/* The default tab stops, as a mask over columns. */
#define TERM_TAB_INTERVAL 8

/*
 * Sized for static allocation: ~285 KB. Never put one on the stack.
 */
typedef struct {
	TermCell grid[TERM_ROWS][TERM_COLS];
	TermCell alt[TERM_ROWS][TERM_COLS];
	TermCell sb[TERM_SB_LINES][TERM_COLS];

	int sb_head;  /* index of the oldest retained scrollback line */
	int sb_count; /* valid lines in sb, <= TERM_SB_LINES */

	int cx, cy;
	int top, bot; /* inclusive scroll region */

	bool wrap_pending;
	bool autowrap;
	bool cursor_visible;
	bool alt_active;
	bool insert_mode; /* IRM: printing shifts the rest of the line right */
	bool tab_stop[TERM_COLS];

	int8_t cur_fg, cur_bg;
	uint8_t cur_attr;

	/* DECSC/DECRC slot. */
	int saved_cx, saved_cy;
	int8_t saved_fg, saved_bg;
	uint8_t saved_attr;

	/* Separate slot for the ?1049 alternate-screen round trip. */
	int alt_cx, alt_cy;
	int8_t alt_fg, alt_bg;
	uint8_t alt_attr;
} Term;

void term_init(Term *t);
void term_reset(Term *t);

/* Writes a printable codepoint at the cursor, advancing it. */
void term_putc(Term *t, uint32_t ch);

void term_lf(Term *t); /* index: down one line, scrolling at the region bottom */
void term_ri(Term *t); /* reverse index: up one line, scrolling at the region top */
void term_bs(Term *t);
void term_cr(Term *t);
void term_tab(Term *t, int count);
void term_back_tab(Term *t, int count);
void term_set_cursor(Term *t, int col, int row);
void term_move_cursor(Term *t, int dcol, int drow);

void term_scroll_up(Term *t, int top, int bot, int n);
void term_scroll_down(Term *t, int top, int bot, int n);
void term_set_scroll_region(Term *t, int top, int bot);

void term_erase_display(Term *t, int mode);
void term_erase_line(Term *t, int mode);
void term_erase_chars(Term *t, int n);
void term_insert_lines(Term *t, int n);
void term_delete_lines(Term *t, int n);
void term_insert_chars(Term *t, int n);
void term_delete_chars(Term *t, int n);

void term_set_alt_screen(Term *t, bool on);
void term_save_cursor(Term *t);
void term_restore_cursor(Term *t);

void term_set_insert_mode(Term *t, bool on);

void term_set_tab_stop(Term *t, int col);
void term_clear_tab_stops(Term *t, int mode);

/* Current rendition, as set by SGR. */
void term_sgr_reset(Term *t);
void term_sgr_set_fg(Term *t, int color);
void term_sgr_set_bg(Term *t, int color);
void term_sgr_set_attr(Term *t, uint8_t attr, bool on);

/* Nearest palette entry, for SGR 38/48 extended colour. */
int term_nearest_color(int r, int g, int b);

/* Scrollback access. Line 0 is the oldest retained line. */
int term_sb_count(const Term *t);
const TermCell *term_sb_line(const Term *t, int index);
void term_sb_clear(Term *t);

/*
 * Resolves one screen row to the line it should display, given how far the
 * view is scrolled back. offset == 0 is the live screen. Returns NULL when the
 * row falls outside both scrollback and the visible grid.
 */
const TermCell *term_view_line(const Term *t, int offset, int row);

#endif /* VITASSH_TERM_H */
