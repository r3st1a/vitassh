/*
 * vita2d drawing: the glyph atlas, the terminal grid, and plain rectangles.
 *
 * Colours are the 0xAABBGGRR values vita2d expects.
 */
#ifndef VITASSH_RENDER_H
#define VITASSH_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#include "term.h"

#define SCREEN_W 960
#define SCREEN_H 544

/* The 8x8 glyphs are drawn at 1:1; CELL_H adds two rows of leading. */
#define CELL_W 8
#define CELL_H 10

/* 120x51 cells, leaving a 34px status bar at the bottom. */
#define TERM_PX_W (TERM_COLS * CELL_W)
#define TERM_PX_H (TERM_ROWS * CELL_H)
#define STATUS_Y TERM_PX_H
#define STATUS_H (SCREEN_H - STATUS_Y)

int render_init(void);
void render_shutdown(void);

/* Clears to the default background first. render_dialog_update() must be
 * called before the next render_begin(). */
void render_begin(void);
void render_end(void);

#define RENDER_COLOR_BLACK 0xFF000000u
#define RENDER_COLOR_WHITE 0xFFFFFFFFu
#define RENDER_COLOR_PANEL 0xF0181818u
#define RENDER_COLOR_PANEL_EDGE 0xFF606060u
#define RENDER_COLOR_SELECT 0xFF406080u
#define RENDER_COLOR_DIM 0xFF909090u
#define RENDER_COLOR_ERROR 0xFF4040FFu
#define RENDER_COLOR_OK 0xFF40FF40u
#define RENDER_COLOR_WARN 0xFF40D0FFu

void render_fill(int x, int y, int w, int h, uint32_t color);
void render_frame(int x, int y, int w, int h, uint32_t color, int thickness);

/* scale 1 is 8x8, scale 2 is 16x16. */
void render_text(int x, int y, int scale, uint32_t color, const char *text);
int render_text_width(int scale, const char *text);

/* Palette lookups; TERM_COLOR_DEFAULT resolves to the theme defaults. */
uint32_t render_fg(int index);
uint32_t render_bg(int index);

#define RENDER_CURSOR render_fg(TERM_COLOR_DEFAULT)

/*
 * Draws the visible screen, scrolled back by `offset` lines. `y_offset` shifts
 * the whole grid; the touch keyboard uses a negative value to keep the bottom
 * row visible above itself.
 */
void render_terminal(const Term *t, int offset, bool cursor_on, int y_offset);

/*
 * Hands the frame to the common dialog layer. The on-screen keyboard will not
 * appear unless this runs after render_end(), which is exactly the ordering
 * trick the IME needs.
 */
void render_dialog_update(void);

#endif /* VITASSH_RENDER_H */
